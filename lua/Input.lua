local cqueues = require 'cqueues'
local evdev = require 'evdev'
local k = require 'evdev.constants'

local Input = {
	keycodes = k,

	MKEY_LSHIFT	= 0x010000,
	MKEY_RSHIFT	= 0x020000,
	MKEY_LALT	= 0x040000,
	MKEY_RALT	= 0x080000,
	MKEY_LCTRL	= 0x100000,
	MKEY_RCTRL	= 0x200000,

	-- EV_KEY's value's
	KEYVAL_UP = 0,
	KEYVAL_DOWN = 1,
	KEYVAL_REPEAT = 2,
}

function Input:new(device, keymap, grab)
	o = {
		devname = device,
		keymap = keymap,
		grab = grab,
		onKeyUp = {},
		onKeyDown = {},
		onKeyRepeat = {},
	}
	setmetatable(o, self)
	self.__index = self
	return o
end

function Input:bindKey(key, func)
	self.onKeyUp[key] = func
	self.onKeyDown[key] = func
	self.onKeyRepeat[key] = func
end

function Input:bindKeyUp(key, func)
	self.onKeyUp[key] = func
end

function Input:bindKeyDn(key, func)
	self.onKeyDown[key] = func
end

function Input:run(cq)
	cq:wrap(function()
		print(("opening input device %s"):format(self.devname))
		local dev = evdev.Device(self.devname)

		if self.grab then dev:grab() end

		local modifiermap = {
			[k.KEY_LEFTCTRL]   = Input.MKEY_LCTRL,
			[k.KEY_RIGHTCTRL]  = Input.MKEY_RCTRL,
			[k.KEY_LEFTSHIFT]  = Input.MKEY_LSHIFT,
			[k.KEY_RIGHTSHIFT] = Input.MKEY_RSHIFT,
			[k.KEY_LEFTALT]    = Input.MKEY_LALT,
			[k.KEY_RIGHTALT]   = Input.MKEY_RALT,
		}
		local keyStates = {}
		local modifiers = 0
		local keymap = self.keymap

		while true do
			cqueues.poll(dev)

			local eStamp, eType, eCode, eValue = dev:read()
			--print(self.devname, eStamp, eType, eCode, eValue)

			if eType == evdev.EV_KEY then
				local m = modifiermap[eCode]
				local k = bit32.bor(eCode, modifiers)
				local e = keyStates[k]
				if m then
					if eValue == Input.KEYVAL_UP then
						modifiers = bit32.band(modifiers, bit32.bnot(m))
					else
						modifiers = bit32.bor(modifiers, m)
					end
					for k, e in pairs(keyStates) do
						local f = keymap.onKeyClick[k]
						if f then
							e.value = eValue
							e.duration = eStamp - e.timestamp
							f(e)
						end
						keyStates[e.code] = nil
						keyStates[k] = nil
					end
				elseif e ~= nil or eValue == Input.KEYVAL_DOWN then
					if e == nil then
						e = {
							timestamp = eStamp,
							type = eType,
							code = eCode,
						}
						keyStates[k] = e
					end
					e.value = eValue
					e.duration = eStamp - e.timestamp

					if eValue == Input.KEYVAL_UP then
						local f = keymap.onKeyClick[k]
						if f then f(e) end
						keyStates[e.code] = nil
					end

					local f = keymap.onKey[k]
					if f then f(e) end
				end
			end
		end
	end)
end

return Input
