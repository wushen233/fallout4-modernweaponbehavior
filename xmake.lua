set_xmakever("3.0.0")

set_config("commonlib_ini", true)
set_config("commonlib_json", true)
set_config("commonlib_xbyak", true)

local commonlibf4_path = os.getenv("COMMONLIBF4_PATH")
local commonlibf4_profile = os.getenv("COMMONLIBF4_PROFILE")
if commonlibf4_path and commonlibf4_path ~= "" then
    includes(commonlibf4_path)
    if commonlibf4_profile and commonlibf4_profile ~= "" then
        print("[commonlibf4] profile=" .. commonlibf4_profile)
    end
else
    includes("lib/commonlibf4")
end

set_project("Modern Weapon Behavior")
set_version("2.5.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_defines("COMMONLIB_RUNTIMECOUNT=3")

target("MWB")
    add_rules("commonlibf4.plugin", {
        name = "Modern Weapon Behavior",
        author = "LuBuCake @ 2026",
        description = "All-in-one overhaul of Fallout 4's weapon behaviors, including first and third person.",
    })

    set_symbols("debug")

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
