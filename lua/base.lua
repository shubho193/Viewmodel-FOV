if _G.VMFOV then
	return
end

_G.VMFOV = {
	mod_path = ModPath,
	save_path = SavePath .. "ViewmodelFOV.json",
	settings = {
		enabled = true,
		gun_fov = 75,
		apply_in_ads = false
	}
}

function VMFOV:load()
	local file = io.open(self.save_path, "r")
	if not file then
		return
	end

	local ok, data = pcall(json.decode, file:read("*all"))
	file:close()

	if ok and type(data) == "table" then
		for key, value in pairs(data) do
			if self.settings[key] ~= nil then
				self.settings[key] = value
			end
		end
	end
end

function VMFOV:save()
	local file = io.open(self.save_path, "w+")
	if file then
		file:write(json.encode(self.settings))
		file:close()
	end
end

VMFOV:load()
