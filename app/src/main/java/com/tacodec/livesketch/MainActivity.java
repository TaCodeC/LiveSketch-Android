package com.tacodec.livesketch;

import android.content.Context;
import android.content.pm.ActivityInfo;
import android.net.nsd.NsdManager;
import android.os.Build;
import android.os.Bundle;
import android.view.InputDevice;

import org.libsdl.app.SDLActivity;

/**
 * Actividad de LiveSketch. Toda la app vive en C++ (liblivesketch.so); SDL se encarga
 * de la ventana, el contexto GL, el input y el ciclo de vida.
 */
public class MainActivity extends SDLActivity {

    // Mensaje de liblivesketch (SDL_SendAndroidMessage): ventana de gama amplia (1) mientras
    // el lienzo es Display P3, o normal (0). Ver App/DisplayGamut.cpp.
    private static final int COMMAND_COLOR_MODE = COMMAND_USER + 1;

    // El SDK de NDI para Android usa el servicio NSD del sistema para el descubrimiento y
    // exige que exista una instancia de NsdManager mientras haya senders activos.
    private NsdManager mNsdManager;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "livesketch" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        mNsdManager = (NsdManager) getSystemService(Context.NSD_SERVICE);

        // Entrega cada muestra del lápiz en cuanto llega, sin agruparlas por frame:
        // trazos más suaves y con menos latencia (Android 11+).
        if (Build.VERSION.SDK_INT >= 30 && mSurface != null) {
            mSurface.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_POINTER);
        }
    }

    @Override
    protected boolean onUnhandledMessage(int command, Object param) {
        if (command == COMMAND_COLOR_MODE) {
            boolean wide = (param instanceof Integer) && ((Integer) param != 0);
            getWindow().setColorMode(wide ? ActivityInfo.COLOR_MODE_WIDE_COLOR_GAMUT : ActivityInfo.COLOR_MODE_DEFAULT);
            return true;
        }
        return super.onUnhandledMessage(command, param);
    }
}
