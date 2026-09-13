#pragma once
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// CPU provenance only, never a claim that a GPU submission has completed.
// Tokens identify immutable depth snapshots, not recyclable backbuffer indices.
namespace ReShadeDepth
{
	struct FrameStamp
	{
		uint64_t serial = 0, engineFrame = 0, backendFrame = 0;
		bool generated = false;
		bool derived = false; // Parent of a processed/pacer batch, not exact generated geometry.
		bool operator==(const FrameStamp&) const = default;
	};

	class FrameGraph
	{
	public:
		using Resource = uint64_t;
		using Stamp = std::optional<FrameStamp>;
		struct Copy
		{
			Resource source, destination;
			Stamp recordedSource;
			bool full;
			bool writeHint = false;
			bool applicationSource = false;
		};
		Stamp Find(Resource resource) const
		{
			const auto it = colors.find(resource);
			return it == colors.end() ? Stamp{} : it->second;
		}
		void Set(Resource resource, Stamp stamp)
		{
			if (!resource) return;
			if (stamp) colors[resource] = *stamp;
			else colors.erase(resource);
		}
		Copy Record(Resource source, Resource destination, bool full) const
		{
			return { source, destination, Find(source), full, false, applications.contains(source) };
		}
		void SetApplication(Resource resource, Stamp stamp)
		{
			if (resource) applications.insert(resource);
			Set(resource, stamp);
		}
		Copy WriteHint(Resource destination) const { return { 0, destination, {}, false, true }; }
		// A source replaced between recording and submission is ambiguous: neither
		// the old label nor an arbitrary newer label proves which pixels were read.
		// A preceding copy in this submission is authoritative for a local chain.
		void Execute(const std::vector<Copy>& copies)
		{
			// Streamline's pacer submits processing + one application's color copy
			// + generated output copy in one list. The other outputs in that SAME
			// submission belong to its single parent frame, not the newest app frame.
			// Two distinct/changed parents are ambiguous and must not be guessed.
			Stamp parent;
			bool ambiguous = false;
			for (const auto& copy : copies) if (copy.applicationSource && copy.full) {
				const auto current = Find(copy.source);
				if (!current || (copy.recordedSource && copy.recordedSource != current)) ambiguous = true;
				else if (parent && parent->serial != current->serial) ambiguous = true;
				else parent = current;
			}
			if (ambiguous) parent.reset();
			std::unordered_map<Resource, Stamp> local;
			for (const auto& copy : copies) {
				Stamp stamp;
				if (copy.writeHint) {
					if (parent) { stamp = parent; stamp->derived = true; }
					// Merely entering a writable state does not change pixels. Keep a
					// full copy made earlier in this list even without a batch parent.
					else if (const auto it = local.find(copy.destination); it != local.end()) stamp = it->second;
				} else if (copy.full) {
					if (const auto it = local.find(copy.source); it != local.end()) stamp = it->second;
					else {
						const auto current = Find(copy.source);
						if (!copy.recordedSource || copy.recordedSource == current) stamp = current;
					}
				}
				local[copy.destination] = stamp;
				Set(copy.destination, stamp);
			}
		}
		void Forget(Resource resource) { colors.erase(resource); applications.erase(resource); }
		void Clear() { colors.clear(); applications.clear(); }
	private:
		std::unordered_map<Resource, FrameStamp> colors;
		std::unordered_set<Resource> applications;
	};
}
