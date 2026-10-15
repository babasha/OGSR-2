#pragma once

// Android-only helpers of the updater (upd_http_android.cpp -> com.ogsr.online.OgsrUpdate).
#ifdef __ANDROID__
// The transfer in flight: its counters into its upd_progress, the cancel flag into Java (call from the menu's thread).
void upd_android_poll();
// The installed APK's versionCode (-1: unknown).
long long upd_android_version_code();
// 0: the system installer is up. 1: the user must first allow this app to install apps (the settings page was opened).
// <0: failed, err says why.
int upd_android_install_apk(const char* path, char* err, int err_size);
// The app starts again ~0.5 s after this process ends.
bool upd_android_schedule_restart();
#endif
