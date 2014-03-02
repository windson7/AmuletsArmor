package.path = './?.lua;./Lua/?.lua;./Lua/AAEngine/?.lua;./Lua/Classes/?.lua';

local color = require "AAEngine/color"
local display = require "AAEngine/display"
local graphics = require "AAEngine/graphics"
local keyboard = require "AAEngine/keyboard"
local keymap = require "AAEngine/keymap"
local mouse = require "AAEngine/mouse"
local pics = require "AAEngine/pics"
local sound = require "AAEngine/sound"
local time = require "AAEngine/time"
local ticker = require "AAEngine/ticker"
local view = require "AAEngine/view"

local titlescreen = require "AAGame/titlescreen"

local config = require "config"

local function startup()
	print "Hello "
end

startup();

require "AAGame/smmain" ;
smmain:update();
smmain:update();
smmain:update();