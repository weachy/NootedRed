includes(path.join(os.getenv("AppleForgePath") or "../AppleForge", "xmake/rules/AppleForge.lua"))

set_project("NootedRed")
set_version("1.2.1")

AppleForge_ApplyDefaults()
set_allowedarchs("x86_64")
set_defaultarchs("x86_64")

-- Put build artifacts in a per-branch directory so switching branches cannot overwrite another
-- branch's kext/zip. Override with NRED_BUILD_BRANCH=<name> when the branch name is not suitable.
local function nred_branch_dir()
    -- tools/build-branch.sh exports this; direct xmake falls back to a shared directory.
    local branch = os.getenv("NRED_BUILD_BRANCH") or "shared"
    branch = branch:gsub("[^%w._-]+", "-"):gsub("^%-+", ""):gsub("%-+$", "")
    if branch == "" then
        branch = "unknown"
    end
    return branch
end



target("NootedRed")
	add_rules("LiluPlugIn")
	set_kind("binary")
	set_languages("cxx23")
	add_cxflags("-Wno-c23-extensions")

	add_files("NootedRed/PenguinWizardry/**.cpp")
	add_files("NootedRed/**.cpp")
	add_includedirs("NootedRed")
	set_values("BundleIdentifier", "org.ChefKiss.NootedRed")
	on_load(function(target)
		local branch = nred_branch_dir()
		local plat = get_config("plat") or os.host()
		local arch = get_config("arch") or "x86_64"
		local mode = get_config("mode") or "release"
		target:set("targetdir", path.join("build", "branches", branch, plat, arch, mode))
	end)
target_end()
