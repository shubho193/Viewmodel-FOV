dofile(ModPath .. "lua/base.lua")

Hooks:Add("LocalizationManagerPostInit", "VMFOV_Localization", function(loc)
	loc:load_localization_file(VMFOV.mod_path .. "loc/english.txt")
end)

Hooks:Add("MenuManagerInitialize", "VMFOV_MenuInit", function()
	MenuCallbackHandler.vmfov_toggle = function(_, item)
		VMFOV.settings[item:name()] = item:value() == "on"
	end

	MenuCallbackHandler.vmfov_slider = function(_, item)
		VMFOV.settings[item:name()] = math.floor(item:value() + 0.5)
	end

	MenuCallbackHandler.vmfov_closed = function()
		VMFOV:save()
	end

	MenuHelper:LoadFromJsonFile(VMFOV.mod_path .. "menu/options.txt", VMFOV, VMFOV.settings)
end)
