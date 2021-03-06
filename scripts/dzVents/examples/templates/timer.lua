return {
	on = {
		timer = {
			'every minute',              -- causes the script to be called every minute
			'every other minute',        -- minutes: xx:00, xx:02, xx:04, ..., xx:58
			'every <xx> minutes',        -- starting from xx:00 triggers every xx minutes
										 -- (0 > xx < 60)
			'every hour',                -- 00:00, 01:00, ..., 23:00  (24x per 24hrs)
			'every other hour',          -- 00:00, 02:00, ..., 22:00  (12x per 24hrs)
			'every <xx> hours',          -- starting from 00:00, triggers every xx
										 -- hours (0 > xx < 24)
			'at 13:45',                  -- specific time
			'at *:45',                   -- every 45th minute in the hour
			'at 15:*',                   -- every minute between 15:00 and 16:00
			'at 12:45-21:15',            -- between 12:45 and 21:15. You cannot use '*'!
			'at 19:30-08:20',            -- between 19:30 and 8:20 then next day
			'at 13:45 on mon,tue',       -- at 13:45 only on Monday en Tuesday (english)
			'every hour on sat',         -- you guessed it correctly
			'at sunset',                 -- uses sunset/sunrise info from Domoticz
			'at sunrise',
			'at sunset on sat,sun',
			'xx minutes before sunset',
			'xx minutes after sunset',
			'xx minutes before sunrise',
			'xx minutes after sunrise',   -- guess ;-)
			'between aa and bb',          -- aa/bb can be a time stamp like 15:44
										 -- aa/bb can be sunrise/sunset
										 -- aa/bb can be 'xx minutes before/after
										 -- sunrise/sunset'
			'at nighttime',              -- between sunset and sunrise
			'at daytime',                -- between sunrise and sunset
			'at civildaytime',           -- between civil twilight start and civil twilight end
			'at civilnighttime',         -- between civil twilight end and civil twilight startœ
			'at daytime on mon,tue',     -- between sunrise and sunset
										 --	only on monday and tuesday

			-- or if you want to go really wild:
			'at nighttime at 21:32-05:44 every 5 minutes on sat, sun',
			'every 10 minutes between 20 minutes before sunset and 30 minutes after sunrise on mon,fri,tue',

			-- or just do it yourself:
			function(domoticz)
				-- you can use domoticz.time to get the current time
				-- note that this function is called every minute!
				-- custom code that either returns true or false
				return true
			end
	   },
    },
	execute = function(domoticz, timer)
		domoticz.log('Timer event was triggered by ' .. timer.trigger, domoticz.LOG_INFO)
	end
}
