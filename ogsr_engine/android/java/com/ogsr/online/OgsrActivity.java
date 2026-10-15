package com.ogsr.online;

import org.libsdl.app.SDLActivity;

// The engine is libmain.so (ogsr_engine/xr_3da/CMakeLists.txt); libc++_shared.so is pulled in by it as a dependency.
public class OgsrActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
