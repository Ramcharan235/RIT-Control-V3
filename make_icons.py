"""Creates all Android launcher icons AND the start-up (splash) screen for RIT Control from 3 PNG files:
   icon-legacy.png, icon-round.png, icon-foreground.png  (1024x1024 each)
   The splash screen shows the RIT car logo instead of the Capacitor logo.
   Run after `npx cap add android`.   Usage: python3 make_icons.py [path-to-res-folder]"""
import os, sys, re, glob
from PIL import Image

RES = sys.argv[1] if len(sys.argv) > 1 else "android/app/src/main/res"
LEGACY = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
ADAPT  = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}

def save(src, size, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.open(src).convert("RGBA").resize((size, size), Image.LANCZOS).save(path)

for d, px in LEGACY.items():
    save("icon-legacy.png", px, f"{RES}/mipmap-{d}/ic_launcher.png")
    save("icon-round.png",  px, f"{RES}/mipmap-{d}/ic_launcher_round.png")
for d, px in ADAPT.items():
    save("icon-foreground.png", px, f"{RES}/mipmap-{d}/rit_icon_fg.png")

os.makedirs(f"{RES}/values", exist_ok=True)
open(f"{RES}/values/rit_icon_colors.xml", "w").write(
 '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="rit_icon_bg">#5E72FF</color>\n</resources>\n')

xml = ('<?xml version="1.0" encoding="utf-8"?>\n'
       '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
       '    <background android:drawable="@color/rit_icon_bg"/>\n'
       '    <foreground android:drawable="@mipmap/rit_icon_fg"/>\n'
       '</adaptive-icon>\n')
os.makedirs(f"{RES}/mipmap-anydpi-v26", exist_ok=True)
for n in ("ic_launcher.xml", "ic_launcher_round.xml"):
    open(f"{RES}/mipmap-anydpi-v26/{n}", "w").write(xml)

# ---------- splash screen: car logo instead of the Capacitor logo ----------
for f in glob.glob(f"{RES}/drawable*/splash.png"):          # remove Capacitor's logo images
    os.remove(f)
os.makedirs(f"{RES}/drawable", exist_ok=True)
fg = Image.open("icon-foreground.png").convert("RGBA").resize((760, 760), Image.LANCZOS)
canvas = Image.new("RGBA", (1152, 1152), (0, 0, 0, 0))      # 288dp canvas (Android 12 splash icon)
canvas.paste(fg, (196, 196), fg)
canvas.save(f"{RES}/drawable/rit_splash_icon.png")
open(f"{RES}/drawable/rit_splash.xml", "w").write(
 '<?xml version="1.0" encoding="utf-8"?>\n'
 '<layer-list xmlns:android="http://schemas.android.com/apk/res/android">\n'
 '    <item><shape android:shape="rectangle"><solid android:color="@color/rit_icon_bg"/></shape></item>\n'
 '    <item android:width="260dp" android:height="260dp" android:gravity="center">\n'
 '        <bitmap android:src="@drawable/rit_splash_icon" android:gravity="fill"/>\n'
 '    </item>\n</layer-list>\n')
st = f"{RES}/values/styles.xml"
if os.path.exists(st):
    x = open(st, encoding="utf-8").read().replace("@drawable/splash", "@drawable/rit_splash")
    if "windowSplashScreenAnimatedIcon" not in x:
        x = re.sub(r'(<style name="AppTheme\.NoActionBarLaunch"[^>]*>)',
                   r'\1\n        <item name="windowSplashScreenBackground">@color/rit_icon_bg</item>'
                   r'\n        <item name="windowSplashScreenAnimatedIcon">@drawable/rit_splash_icon</item>', x, count=1)
    open(st, "w", encoding="utf-8").write(x)
    print("Splash screen updated (car logo)")
else:
    print("styles.xml not found - splash not changed")
print("RIT Control icons created in", RES)
