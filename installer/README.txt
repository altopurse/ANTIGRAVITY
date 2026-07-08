========================================================================
ANTIGRAVITY VOICE ENGINE & SOUNDBOARD - USER GUIDE
========================================================================

INSTALLING
----------
1. Run "AntigravityVoiceEngine-Setup.exe" and follow the wizard. It
   installs per-user (no admin needed) and creates Desktop / Start
   Menu shortcuts. If Windows SmartScreen warns about an unknown
   publisher, click "More info" -> "Run anyway".
2. To uninstall later: "Antigravity Voice Engine" in Windows
   Settings > Apps.

OUTPUT SETUP - HOW TO BE HEARD IN DISCORD / GAMES
-------------------------------------------------
The app takes your microphone, applies effects, and plays the result on
an output device. For other apps to "hear" that as a microphone, you
need a virtual audio cable:

1. Install the free VB-CABLE driver from https://vb-audio.com/Cable
   and reboot your PC.
2. Open Antigravity Voice Engine and set:
      Mic Input      = your real microphone
      Primary Output = "CABLE Input (VB-Audio Virtual Cable)"
      Voice Monitor  = your headphones/speakers
3. Press START AUDIO ENGINE.
4. In Discord / Zoom / your game, set the input (microphone) device to
      "CABLE Output (VB-Audio Virtual Cable)".

No virtual cable? Set Primary Output to your headphones to try the
effects locally (others won't hear the processed voice in that mode).

HEARING YOURSELF & THE SOUNDBOARD
---------------------------------
- Soundboard clips ALWAYS play through the Voice Monitor device while
  the engine is running, so you hear what you trigger.
- Enable "Voice Loopback (Monitoring)" to also hear your own processed
  voice through the Voice Monitor device.
- Use the "Monitor Vol" slider to adjust how loud that is.

SOUNDBOARD
----------
- "ADD AUDIO FILE" imports WAV/MP3/OGG/FLAC files; they are copied into
  the app's "sounds" folder and reload automatically at startup.
- Bind global hotkeys per clip with the "Hotkey" button; right-click a
  clip card to clear the hotkey or remove the clip.

TROUBLESHOOTING
---------------
- Crackling/pops: raise "Buffer size (ms)" in the app, or disable
  Exclusive Mode.
- No devices listed: check Windows privacy settings allow microphone
  access, then restart the app.
- Discord echo: make sure Discord's input is CABLE Output, NOT your
  real microphone, and disable Discord's own noise suppression if it
  fights the noise gate.
