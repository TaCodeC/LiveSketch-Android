package com.tacodec.livesketch;

import android.content.Context;
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
}
