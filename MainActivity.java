package com.rit.control;

import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Set;
import org.json.JSONObject;
import com.getcapacitor.BridgeActivity;

/* RIT Control native helpers for the web app:
   1) setImmersive(on)  - FULL SCREEN (hides status + navigation bar) while Car Control is open
   2) discover(ms)      - finds the NodeMCU boards near you in about a second: the phone shouts
                          "RIT_DISCOVER" (UDP port 4210) to the whole Wi-Fi network and every board
                          answers - no IP scanning. Replies go back to the web page. */
public class MainActivity extends BridgeActivity {

  @Override
  public void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);

    if (Build.VERSION.SDK_INT >= 28) {            // draw under the camera notch too
      WindowManager.LayoutParams lp = getWindow().getAttributes();
      lp.layoutInDisplayCutoutMode = (Build.VERSION.SDK_INT >= 30)
          ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
          : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
      getWindow().setAttributes(lp);
    }

    getBridge().getWebView().addJavascriptInterface(new Object() {
      @JavascriptInterface
      public void setImmersive(final boolean on) {
        runOnUiThread(new Runnable() {
          @Override
          public void run() { applyImmersive(on); }
        });
      }

      @JavascriptInterface
      public void discover(final int timeoutMs) {
        new Thread(new Runnable() {
          @Override
          public void run() { runDiscovery(timeoutMs); }
        }).start();
      }
    }, "RITNative");
  }

  private void applyImmersive(boolean on) {
    try {
      if (Build.VERSION.SDK_INT >= 30) {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        if (on) {
          c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
          c.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
        } else {
          c.show(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
        }
      } else {
        View d = getWindow().getDecorView();
        if (on) {
          d.setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
              | View.SYSTEM_UI_FLAG_FULLSCREEN
              | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
              | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
              | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
              | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        } else {
          d.setSystemUiVisibility(View.SYSTEM_UI_FLAG_VISIBLE);
        }
      }
    } catch (Exception e) { }
  }

  /* ---------- instant board discovery (UDP broadcast) ---------- */
  private void toJs(final String js) {
    runOnUiThread(new Runnable() {
      @Override
      public void run() {
        try { getBridge().getWebView().evaluateJavascript(js, null); } catch (Exception e) { }
      }
    });
  }

  private Set<String> discoveryTargets() {
    Set<String> t = new LinkedHashSet<String>();
    t.add("255.255.255.255");
    t.add("192.168.4.1");                              // the board's own hotspot
    try {
      for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
        if (!ni.isUp() || ni.isLoopback()) continue;
        for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
          InetAddress b = ia.getBroadcast();
          if (b != null) t.add(b.getHostAddress());     // e.g. 192.168.1.255
        }
      }
    } catch (Exception e) { }
    return t;
  }

  private void runDiscovery(int timeoutMs) {
    DatagramSocket sock = null;
    try {
      sock = new DatagramSocket(null);
      sock.setReuseAddress(true);
      sock.bind(new InetSocketAddress(0));
      sock.setBroadcast(true);
      sock.setSoTimeout(120);
      byte[] msg = "RIT_DISCOVER".getBytes("UTF-8");
      Set<String> targets = discoveryTargets();
      Set<String> seen = new HashSet<String>();
      byte[] buf = new byte[1024];
      long end = System.currentTimeMillis() + timeoutMs, next = 0;
      int sent = 0;
      while (System.currentTimeMillis() < end) {
        if (sent < 4 && System.currentTimeMillis() >= next) {   // shout a few times (UDP can drop one)
          for (String ip : targets) {
            try { sock.send(new DatagramPacket(msg, msg.length, InetAddress.getByName(ip), 4210)); } catch (Exception e) { }
          }
          sent++; next = System.currentTimeMillis() + 300;
        }
        try {
          DatagramPacket p = new DatagramPacket(buf, buf.length);
          sock.receive(p);
          String ip = p.getAddress().getHostAddress();
          if (seen.add(ip)) {
            String body = new String(p.getData(), 0, p.getLength(), "UTF-8");
            toJs("window.__ritFound&&window.__ritFound(" + JSONObject.quote(ip) + "," + JSONObject.quote(body) + ")");
          }
        } catch (SocketTimeoutException e) { }
      }
    } catch (Exception e) {
    } finally {
      try { if (sock != null) sock.close(); } catch (Exception e) { }
      toJs("window.__ritDone&&window.__ritDone()");
    }
  }
}
