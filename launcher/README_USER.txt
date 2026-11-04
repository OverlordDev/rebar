Rebar: run Apple Silicon (arm64) apps on a Mac with an Intel or AMD processor

How to use
1. Move Rebar.app to Applications (or anywhere) and open it. If macOS says the app is damaged or from an unidentified developer
   (it is not signed, that is normal), run this in Terminal:
       xattr -dr com.apple.quarantine /Applications/Rebar.app
   (or: System Settings > Privacy & Security > "Open Anyway").
2. Rebar looks in Applications for apps that only have an arm64 version (what the Mac cannot run by itself) and lists them.
3. Click "Launch" next to an app. The first launch can take up to a minute.
4. The gear next to an app opens its own settings: GPU acceleration, fast mode, arguments, environment variables.
   Changes take effect on the next launch.
5. If an app is not in the list (for example it sits in an open .dmg), click "Add..." and choose the app or a folder.

Important: rebar is an emulator and does not work with every app. If an app closes right away, open its settings > "Show log".
macOS 11 or newer is required (some apps need more, for example VS Code needs macOS 12).

If you clicked "Launch" and there is no window
The app may still be running from last time (without a window): the new launch then hands over to it and exits at once, so nothing seems to happen.
The launcher now sees such running instances: they say "Running" or "Running, no window", with the buttons "Stop" (close everything left of the app)
and "Restart" (close and start again). While an app is still starting it says "Starting...": the first launch can take up to a minute.

Tip for VS Code: scrolling text
By default the editor scrolls at about 50 frames per second on screen (on a strong machine under emulation). To get 60 and more,
the switch "Fast scrolling (lighter editor)" in the VS Code settings (the gear next to the app) is on by default: when the app starts it adds to your settings.json
a block between the lines "// >>> rebar" and "// <<< rebar"; turn the switch off and the block is removed (a copy of the file from before the first change: settings.json.rebar-backup). By hand the same is (Settings > search by name, or in settings.json):
    "editor.minimap.enabled": false,
    "editor.stickyScroll.enabled": false,
    "breadcrumbs.enabled": false,
    "editor.bracketPairColorization.enabled": false,
    "editor.guides.bracketPairs": false,
    "editor.occurrencesHighlight": "off",
    "editor.selectionHighlight": false,
    "editor.renderLineHighlight": "none"
The minimap alone gives about +4 frames per second. This does not speed up the emulator, it is just less work per frame.
