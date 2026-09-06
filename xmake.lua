set_xmakever("3.0.0")

-- include subprojects
includes("lib/commonlibf4")

-- set project constants
set_project("Upscaling")
set_version("1.5.2")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
set_arch("x64")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_defines("COMMONLIB_RUNTIMECOUNT=3")

add_requires("directxtk", "directx-headers", "magic_enum", "simpleini")

-- define targets
target("Upscaling")
    set_kind("shared")
    set_filename("Upscaling.dll")
    set_extension(".dll")
    add_deps("commonlibf4")
    add_packages("directxtk", "directx-headers", "magic_enum", "simpleini")

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_headerfiles("include/**.h", "include/**.hpp")
    add_includedirs("src", "include")
    add_includedirs("extern/Streamline/include")
    add_includedirs("extern/Streamline/external/ngx-sdk/include")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/api/include")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/backend/dx12")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/include")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include/dx12")
    set_pcxxheader("include/PCH.h")

    add_links("d3d11", "d3d12", "d3dcompiler", "gdi32", "bcrypt", "version")

    add_linkdirs("include/detours/Release", { public = false })
    add_links("detours")

    add_linkdirs("extern/FidelityFX-SDK/Kits/FidelityFX/signedbin", { public = false })
    add_links("amd_fidelityfx_loader_dx12")
    add_links("delayimp")
    add_shflags("/DELAYLOAD:amd_fidelityfx_loader_dx12.dll", { force = true })

    add_defines("_AMD64_", "_WINDOWS", "_UNICODE")
    add_cxxflags("cl::/bigobj", "cl::/MP", "cl::/permissive-", "cl::/Zc:__cplusplus", "cl::/arch:AVX")

    after_build(function (target)
        local signedbin = path.join(os.projectdir(), "extern", "FidelityFX-SDK", "Kits", "FidelityFX", "signedbin")
        os.cp(path.join(signedbin, "amd_fidelityfx_loader_dx12.dll"), target:targetdir())
        os.cp(path.join(signedbin, "amd_fidelityfx_upscaler_dx12.dll"), target:targetdir())
        os.cp(path.join(signedbin, "amd_fidelityfx_framegeneration_dx12.dll"), target:targetdir())
    end)
