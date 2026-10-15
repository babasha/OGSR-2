package com.ogsr.online;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Build;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

// The Java half of the updater (ogsr_engine/xr_3da/update/upd_http_android.cpp, xr_update.h). Called from the engine's
// own threads through JNI; every method blocks and returns a code, lastError() says why when it is not success.
public class OgsrUpdate {
    private static final String TAG = "ogsr";
    private static volatile String sLastError = "";

    public static String lastError() {
        return sLastError;
    }

    private static Context ctx() {
        return SDLActivity.getContext();
    }

    // GET url into dst. progress: [0] bytes written, [1] Content-Length (-1 unknown) -- both ours to write; [2] != 0
    // means "stop" (the engine writes it). Returns the HTTP status (200 = the whole body is in dst), or -1.
    public static int download(String url, String dst, long[] progress) {
        HttpURLConnection c = null;
        try {
            // HttpURLConnection follows redirects only within one protocol: GitHub's are https -> https.
            URL u = new URL(url);
            for (int hop = 0; hop < 8; ++hop) {
                c = (HttpURLConnection) u.openConnection();
                c.setInstanceFollowRedirects(false);
                c.setConnectTimeout(10000);
                c.setReadTimeout(30000);
                c.setRequestProperty("User-Agent", "ogsr-updater/1 (android)");
                c.setRequestProperty("Accept-Encoding", "identity");
                int code = c.getResponseCode();
                if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                    String loc = c.getHeaderField("Location");
                    c.disconnect();
                    c = null;
                    if (loc == null) {
                        sLastError = "HTTP " + code + " without Location";
                        return -1;
                    }
                    u = new URL(u, loc);
                    continue;
                }
                if (code != 200) {
                    sLastError = "HTTP " + code;
                    return code;
                }
                progress[1] = c.getContentLengthLong();
                progress[0] = 0;
                try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(dst)) {
                    byte[] buf = new byte[256 * 1024];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        if (progress[2] != 0) {
                            sLastError = "cancelled";
                            return -1;
                        }
                        out.write(buf, 0, n);
                        progress[0] += n;
                    }
                }
                if (progress[1] >= 0 && progress[0] != progress[1]) {
                    sLastError = "connection lost (" + progress[0] + " of " + progress[1] + ")";
                    return -1;
                }
                return 200;
            }
            sLastError = "too many redirects";
            return -1;
        } catch (java.net.UnknownHostException e) {
            sLastError = "offline (" + e.getMessage() + ")";
            return -1;
        } catch (Exception e) {
            sLastError = e.toString();
            Log.w(TAG, "OgsrUpdate.download " + url + ": " + e);
            return -1;
        } finally {
            if (c != null)
                c.disconnect();
        }
    }

    public static long versionCode() {
        try {
            Context cx = ctx();
            PackageInfo pi = cx.getPackageManager().getPackageInfo(cx.getPackageName(), 0);
            return Build.VERSION.SDK_INT >= 28 ? pi.getLongVersionCode() : pi.versionCode;
        } catch (Exception e) {
            sLastError = e.toString();
            return -1;
        }
    }

    private static final String ACTION_INSTALL_STATUS = "com.ogsr.online.INSTALL_STATUS";
    private static boolean sReceiverRegistered;

    // Hands the APK to the system installer (PackageInstaller session). 0 = the installer is up (it asks the player to
    // confirm; when it finishes, Android stops this process). 1 = this app may not install apps yet: the settings page
    // for that was opened, the player comes back and presses again. -1 = failed.
    public static int installApk(String path) {
        try {
            final Context cx = ctx();
            if (Build.VERSION.SDK_INT >= 26 && !cx.getPackageManager().canRequestPackageInstalls()) {
                Intent s = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + cx.getPackageName()));
                s.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                cx.startActivity(s);
                sLastError = "allow installing apps for OGSR, then press again";
                return 1;
            }
            synchronized (OgsrUpdate.class) {
                if (!sReceiverRegistered) {
                    // The session's result comes back as a broadcast; PENDING_USER_ACTION carries the confirmation
                    // screen, which only we can start.
                    BroadcastReceiver r = new BroadcastReceiver() {
                        @Override
                        public void onReceive(Context c, Intent i) {
                            int st = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                            if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                                Intent confirm = (Intent) i.getParcelableExtra(Intent.EXTRA_INTENT);
                                if (confirm != null) {
                                    confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                                    c.startActivity(confirm);
                                }
                            } else {
                                Log.i(TAG, "OgsrUpdate: install status " + st + " " + i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
                            }
                        }
                    };
                    IntentFilter f = new IntentFilter(ACTION_INSTALL_STATUS);
                    if (Build.VERSION.SDK_INT >= 33)
                        cx.registerReceiver(r, f, Context.RECEIVER_EXPORTED);
                    else
                        cx.registerReceiver(r, f);
                    sReceiverRegistered = true;
                }
            }
            PackageInstaller pi = cx.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            sp.setAppPackageName(cx.getPackageName());
            File apk = new File(path);
            sp.setSize(apk.length());
            int id = pi.createSession(sp);
            try (PackageInstaller.Session s = pi.openSession(id)) {
                try (InputStream in = new FileInputStream(apk); OutputStream out = s.openWrite("base.apk", 0, apk.length())) {
                    byte[] buf = new byte[256 * 1024];
                    int n;
                    while ((n = in.read(buf)) > 0)
                        out.write(buf, 0, n);
                    s.fsync(out);
                }
                Intent st = new Intent(ACTION_INSTALL_STATUS).setPackage(cx.getPackageName());
                int fl = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
                PendingIntent pi2 = PendingIntent.getBroadcast(cx, id, st, fl);
                s.commit(pi2.getIntentSender());
            }
            return 0;
        } catch (Exception e) {
            sLastError = e.toString();
            Log.w(TAG, "OgsrUpdate.installApk: " + e);
            return -1;
        }
    }

    // The game starts again shortly after this process ends (the engine exits right after calling this): an alarm
    // owned by the system, so it survives the process.
    public static boolean scheduleRestart() {
        try {
            Context cx = ctx();
            Intent i = cx.getPackageManager().getLaunchIntentForPackage(cx.getPackageName());
            if (i == null)
                return false;
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            PendingIntent p = PendingIntent.getActivity(cx, 0, i, PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            AlarmManager am = (AlarmManager) cx.getSystemService(Context.ALARM_SERVICE);
            am.set(AlarmManager.ELAPSED_REALTIME, SystemClock.elapsedRealtime() + 700, p);
            return true;
        } catch (Exception e) {
            sLastError = e.toString();
            return false;
        }
    }
}
