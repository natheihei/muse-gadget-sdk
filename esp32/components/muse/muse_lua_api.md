# Lua apps on this gadget

lua.run takes a Lua 5.4 script (under 8 KB) and runs it as an app on the
gadget's touch screen. The firmware draws a top bar with the app's title, a
mic and an X that closes the app. The app draws in the area under the bar,
ui.W x ui.H px, with (0, 0) at its top left. One app runs at a time: lua.run
replaces the one running. An app keeps running, and the screen stays on,
until the X, lua.stop, app.exit() or the next lua.run.

lua.run replies about a second after the script's top level finishes: ok
with the log so far, or the Lua error with its line ("app:12: ..."). Fix the
script and send it again. An error later, in a callback, stops the app, shows
on the screen and is in lua.status.

## Voice

The person can talk to you while an app runs: the top bar keeps a mic. Give
the app's actions names with app.expose{...}, such as next, back and the
current step, and call them with lua.call {name, args} when the person asks
("next step", "what step am I on?"). lua.run's reply and lua.status list
them. What the function returns comes back to you as JSON. If it raises an
error, lua.call replies with it and the app keeps running. One call runs at
a time: another sent before it replies is refused as busy.

## Keep your apps

The gadget doesn't keep apps: one is gone once it's closed or the gadget
restarts. Keep them yourself, so you can change one or show it again later:

- Save each app as a file in your own workspace before you send it, such as
  gadget-apps/scrambled-eggs.lua. Send lua.run the script as always, with
  that path in a source parameter beside it.
- To change the app on the screen, call lua.status for its source, edit that
  file, and send it again. Don't write it over from scratch.
- To show an app again, send the saved file.
- Put the data at the top (a recipe's steps, say) and the code under it, so
  one app can be reused for new data: copy the file and change the data.

## Rules

- Build the screen once, at the top level, and change widgets with w:set{}
  in callbacks. Don't recreate widgets on every tick.
- Never loop waiting for time to pass. A callback that runs over 0.5 s (the
  top level over 3 s) is stopped with an error. Use timers.
- Measure time with time.ms(), never by counting timer ticks. For a
  countdown, keep a deadline and show math.max(0, deadline - time.ms()).
- Fonts have ASCII and the degree sign. Curly quotes, dashes, fractions
  and ellipses are turned into ASCII. When lua.help's cjk is true, Chinese
  and Japanese show too, as 16 px characters at any size, so give text in
  them size 16 or 20. Korean and other characters don't show.
- Standard libraries: string, table, math, utf8, coroutine. There is no io,
  os, require or load.

## API

    log(...)  print(...)      add a line to the log (lua.run, lua.status)

    app.title(text)           the top bar's title (lua.run's title sets it first)
    app.exit()                close the app once this callback returns
    app.on_exit(fn)           fn() runs once when the app is closed or replaced
    app.expose{name = fn, ...}  what lua.call {name, args} can call: fn(args...),
                              and its return value goes back to Muse as JSON

    ui.W, ui.H                the app's area in px
    ui.clear([bg])            delete every widget; background colour, default 0x000000
    ui.label{...}   ui.button{...}   ui.rect{...}   ui.bar{...}   ui.arc{...}
                              -> widget, with these fields (all optional):
      x, y, w, h    position and size in px. A label without w is as wide as
                    its text; with w it wraps, and with h too it ends in "..."
                    if the text doesn't fit.
      text          label or button text; "\n" breaks a line
      size          font size in px: 14, 16, 20, 28 or 48 (default 20)
      color         0xRRGGBB: text for labels and buttons, the fill for rect,
                    bar and arc (default white, or 0x7C5CFF for bar and arc)
      bg            label or button background (button default 0x2A2540),
                    bar or arc track (default 0x2A2540)
      align         "left", "center" or "right": text in its box
      radius        corner radius for button, rect and bar
      value         bar or arc fill, 0 to 100
      width         arc line width, default 12
      hidden        true hides the widget
      on_press      button only: fn() when it's tapped
    w:set{...}                change any field but on_press
    w:delete()
    ui.on_tap(fn)             fn(x, y) on a tap in the app's area but not on a button
    ui.on_swipe(fn)           fn(dir) on a swipe anywhere in the app's area; dir is
                              where the finger moved: "left", "right", "up", "down"

    timer.setTimeout(ms, fn) -> id    fn() once, after ms
    timer.setInterval(ms, fn) -> id   fn() every ms (at least 50)
    timer.clear(id) -> true if it was pending
    time.ms()                 milliseconds since boot, an integer that only goes up

    sound.beep{freq=880, ms=150, count=1, gap=120}
                              beeps on the speaker; waits while Muse is talking
    json.encode(value) -> string      json.decode(string) -> value

Limits: 1 MB of Lua memory, 200 widgets, 32 timers.

## Example: steps with a timer

    local steps = {
      {text = "Boil 2 L of salted water"},
      {text = "Cook the pasta", secs = 600},
      {text = "Drain and toss with the sauce"},
    }
    local i, deadline, rung = 1, nil, false
    app.title("Pasta")
    local count = ui.label{x = 16, y = 12, size = 16, color = 0x9A93B8}
    local body = ui.label{x = 16, y = 40, w = ui.W - 32, h = 120, size = 28}
    local clock = ui.label{x = 0, y = 160, w = ui.W, size = 48, align = "center"}

    local function render()
      local s = steps[i]
      count:set{text = ("Step %d of %d"):format(i, #steps)}
      body:set{text = s.text}
      if not s.secs then clock:set{hidden = true} return end
      local left = s.secs
      if deadline then left = math.max(0, (deadline - time.ms()) // 1000) end
      clock:set{hidden = false, text = ("%d:%02d"):format(left // 60, left % 60),
                color = (deadline and left == 0) and 0x3DDC84 or 0xFFFFFF}
    end

    local function next_step()
      if steps[i].secs and not deadline then
        deadline, rung = time.ms() + steps[i].secs * 1000, false
      elseif i < #steps then
        i, deadline = i + 1, nil
      end
      render()
    end

    local function back()
      if deadline then deadline = nil elseif i > 1 then i = i - 1 end
      render()
    end

    ui.button{x = 16, y = 228, w = 216, h = 48, text = "Back", on_press = back}
    ui.button{x = 248, y = 228, w = 216, h = 48, text = "Next", on_press = next_step}
    ui.on_swipe(function(dir)
      if dir == "left" then next_step() elseif dir == "right" then back() end
    end)
    app.expose{next = next_step, back = back,
      step = function() return {step = i, of = #steps, text = steps[i].text} end}
    timer.setInterval(250, function()
      if deadline and not rung and time.ms() >= deadline then
        rung = true
        sound.beep{count = 3}
      end
      if deadline then render() end
    end)
    render()
