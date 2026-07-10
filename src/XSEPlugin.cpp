#include "DX11Hooks.h"
#include "Upscaling.h"

#include "ENB/ENBSeriesAPI.h"

bool enbLoaded = false;

void InitializeLog()
{
#ifndef NDEBUG
	const auto level = spdlog::level::trace;
#else
	const auto level = spdlog::level::info;
#endif

	spdlog::default_logger()->set_level(level);
	spdlog::default_logger()->flush_on(spdlog::level::info);
	spdlog::set_pattern("%v"s);
}

F4SE_PLUGIN_VERSION = []() noexcept {
	F4SE::PluginVersionData data{};

	data.PluginVersion(Plugin::VERSION);
	data.PluginName(Plugin::NAME.data());
	data.AuthorName("");
	data.UsesAddressLibraryNG(true);
	data.UsesAddressLibraryAE(true);
	data.UsesSigScanning(false);
	data.IsLayoutDependentNG(true);
	data.IsLayoutDependentAE(true);
	data.HasNoStructUse(false);
	data.CompatibleVersions({ F4SE::RUNTIME_1_10_163, F4SE::RUNTIME_1_10_984, F4SE::RUNTIME_LATEST });

	return data;
}();

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface*, F4SE::PluginInfo* a_info)
{
	a_info->name = Plugin::NAME.data();
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->version = 0;
	return true;
}

#ifndef NDEBUG
void AddDebugInformation()
{
	auto rendererData = RE::BSGraphics::GetRendererData();

	for (uint32_t i = 0; i < 101; i++) {
		if (auto texture = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[i].texture)) {
			auto name = std::format("RT {}", i);
			texture->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
		}
		if (auto srView = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[i].srView)) {
			auto name = std::format("SRV {}", i);
			srView->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
		}
		if (auto rtView = reinterpret_cast<ID3D11RenderTargetView*>(rendererData->renderTargets[i].rtView)) {
			auto name = std::format("RTV {}", i);
			rtView->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
		}
		if (auto uaView = reinterpret_cast<ID3D11UnorderedAccessView*>(rendererData->renderTargets[i].uaView)) {
			auto name = std::format("UAV {}", i);
			uaView->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
		}
	}

	for (uint32_t i = 0; i < 13; i++) {
		if (auto texture = reinterpret_cast<ID3D11Texture2D*>(rendererData->depthStencilTargets[i].texture)) {
			auto name = std::format("DEPTH RT {}", i);
			texture->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
		}
		if (auto srViewDepth = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[i].srViewDepth)) {
			auto name = std::format("DS VIEW {}", i);
			srViewDepth->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
		}
	}
}
#endif

void OnInit(F4SE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case F4SE::MessagingInterface::kPostPostLoad:
		Upscaling::InstallHighFPSPhysicsFixCompatibility();
		break;
	case F4SE::MessagingInterface::kGameDataReady:
	{
		logger::info("Data loaded");
		Upscaling::GetSingleton()->OnDataLoaded();
#ifndef NDEBUG
		AddDebugInformation();
#endif
	}
	break;
	default:
		break;
	}
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, { .logName = Plugin::NAME.data() });

#ifndef NDEBUG
	while (!IsDebuggerPresent()) {};
#endif

	InitializeLog();

	auto& trampoline = REL::GetTrampoline();
	trampoline.create(4096);

	if (ENB_API::RequestENBAPI()) {
		logger::info("ENB detected");
		enbLoaded = true;
	} else {
		logger::info("ENB not detected");
	}

	DX11Hooks::Install();
	Upscaling::InstallHooks();

	const auto messaging = F4SE::GetMessagingInterface();
	messaging->RegisterListener(OnInit);

	return true;
}
