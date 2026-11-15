dofile(ModPath .. "lua/base.lua")

local MIN_SCALE = 1
local MAX_SCALE = 4
local MASK_FADE_BACK = 0.5
local COLLECT_ON_FRAME = {
	[1] = true,
	[15] = true,
	[60] = true,
	[120] = true
}

local ids_light = Idstring("light")

local function load_native()
	if VMFOV.native ~= nil then
		return VMFOV.native
	end

	local first, second = blt.load_native(VMFOV.mod_path .. "native/vmfov_native.dll")

	VMFOV.native = type(first) == "table" and first or type(second) == "table" and second or false

	if not VMFOV.native then
		log("[ViewmodelFOV] native plugin failed to load")
	end

	return VMFOV.native
end

function FPCameraPlayerBase:_vmfov_target_scale()
	local settings = VMFOV.settings

	if not settings.enabled or _G.IS_VR or managers.player:current_state() == "driving" then
		return 1
	end

	local scale = math.tan(self._fov.fov * 0.5) / math.tan(settings.gun_fov * 0.5)

	if not settings.apply_in_ads then
		local state = self._parent_movement_ext and self._parent_movement_ext._current_state
		local state_data = state and state._state_data
		local ads_weight = state_data and state_data.steelsight_weight or 0

		scale = math.lerp(scale, 1, ads_weight)
	end

	return scale
end

local function weapon_units(weapon)
	local base = weapon:base()
	local units = {}

	units[weapon:key()] = weapon

	for _, part in pairs(base._parts or {}) do
		if alive(part.unit) then
			units[part.unit:key()] = part.unit
		end
	end

	for _, charm in pairs(base.charm_data and base:charm_data() or {}) do
		if alive(charm.unit) then
			units[charm.unit:key()] = charm.unit
		end
	end

	return units
end

local function owner_of(object, units, fallback)
	for _, unit in pairs(units) do
		if unit:get_object(object:name()) == object then
			return unit
		end
	end

	return fallback
end

local function add_entry(entries, unit, object)
	if object and object:parent() then
		table.insert(entries, {
			unit = unit,
			object = object,
			home = mvector3.copy(object:local_position())
		})
	end
end

local function collect_from_gun(entries, gun)
	local base = gun:base()
	local units = weapon_units(gun)

	for _, unit in pairs(units) do
		for _, light in ipairs(unit:get_objects_by_type(ids_light) or {}) do
			add_entry(entries, unit, light)
		end
	end

	local muzzle = base._muzzle_effect_table and base._muzzle_effect_table.parent
	local shell = base._obj_shell_ejection

	if muzzle then
		add_entry(entries, owner_of(muzzle, units, gun), muzzle)
	end

	if shell then
		add_entry(entries, owner_of(shell, units, gun), shell)
	end
end

local function collect_objects(weapon)
	local entries = {}

	collect_from_gun(entries, weapon)

	local second_gun = weapon:base()._second_gun

	if alive(second_gun) then
		collect_from_gun(entries, second_gun)
	end

	return entries
end

local function restore_objects(entries)
	for _, entry in ipairs(entries or {}) do
		if alive(entry.unit) and entry.moved then
			entry.object:set_local_position(entry.home)
			entry.unit:set_moving()

			entry.moved = false
		end
	end
end

local function home_position(entry)
	local parent = entry.object:parent()
	local position = mvector3.copy(entry.home)

	mvector3.rotate_with(position, parent:rotation())
	mvector3.add(position, parent:position())

	return position
end

function FPCameraPlayerBase:_vmfov_move_object(entry, scale)
	local camera = self._parent_unit:camera()
	local camera_position = camera:position()
	local forward = camera:forward()
	local home = home_position(entry)
	local offset = home - camera_position
	local depth = mvector3.dot(offset, forward)

	mvector3.multiply(offset, scale)
	mvector3.add_scaled(offset, forward, depth * (1 - scale))
	mvector3.add(offset, camera_position)

	entry.object:set_local_position(entry.object:parent():to_local(offset))
	entry.unit:set_moving()

	entry.moved = true
end

function FPCameraPlayerBase:_vmfov_update_objects(scale)
	local weapon = self._parent_unit:inventory():equipped_unit()

	if not alive(weapon) then
		return
	end

	local key = weapon:key()

	if key ~= self._vmfov_weapon then
		restore_objects(self._vmfov_objects)

		self._vmfov_weapon = key
		self._vmfov_objects = {}
		self._vmfov_frames = 0
	end

	self._vmfov_frames = self._vmfov_frames + 1

	local muzzle_table = weapon:base()._muzzle_effect_table
	local muzzle = muzzle_table and muzzle_table.parent
	local muzzle_changed = muzzle ~= self._vmfov_muzzle

	if COLLECT_ON_FRAME[self._vmfov_frames] or muzzle_changed then
		restore_objects(self._vmfov_objects)

		self._vmfov_muzzle = muzzle
		self._vmfov_objects = collect_objects(weapon)
	end

	if scale <= MIN_SCALE then
		restore_objects(self._vmfov_objects)

		return
	end

	for _, entry in ipairs(self._vmfov_objects) do
		if alive(entry.unit) then
			self:_vmfov_move_object(entry, scale)
		end
	end
end

Hooks:PostHook(FPCameraPlayerBase, "update", "VMFOV_Update", function (self, unit, t, dt)
	if not alive(self._parent_unit) then
		return
	end

	local native = load_native()

	if native then
		local scale = self:_vmfov_target_scale()

		local state = self._parent_movement_ext and self._parent_movement_ext._current_state
		if alive(self._mask_unit) or (state and state._equipping_mask) then
			self._vmfov_mask_w = 1
		elseif (self._vmfov_mask_w or 0) > 0 then
			self._vmfov_mask_w = math.max(0, self._vmfov_mask_w - dt / MASK_FADE_BACK)
		end
		local w = self._vmfov_mask_w or 0
		if w > 0 then
			scale = math.lerp(scale, 1, w * w * (3 - 2 * w))
		end

		native.set_scale(scale)

		if not VMFOV.objects_failed and not pcall(self._vmfov_update_objects, self, math.clamp(scale, MIN_SCALE, MAX_SCALE)) then
			VMFOV.objects_failed = true

			pcall(restore_objects, self._vmfov_objects)
			log("[ViewmodelFOV] moving objects on the gun failed, they will stay where they are")
		end
	end
end)
