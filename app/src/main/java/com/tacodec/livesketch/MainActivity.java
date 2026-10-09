package com.tacodec.livesketch;

import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.net.nsd.NsdManager;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.InputDevice;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;

/**
 * Actividad de LiveSketch. Toda la app vive en C++ (liblivesketch.so); SDL se encarga
 * de la ventana, el contexto GL, el input y el ciclo de vida.
 */
public class MainActivity extends SDLActivity {

    private static final String TAG = "LiveSketch";

    // Mensaje de liblivesketch (SDL_SendAndroidMessage): ventana de gama amplia (1) mientras
    // el lienzo es Display P3, o normal (0). Ver App/DisplayGamut.cpp.
    private static final int COMMAND_COLOR_MODE = COMMAND_USER + 1;

    // «Exportar a…»: el selector de documentos del sistema crea el archivo. (Los códigos de los
    // diálogos y permisos de SDL empiezan en 1 y van subiendo de uno en uno.)
    private static final int REQUEST_SAVE_FILE = 0x4C53;

    // El proyecto que se pidió abrir desde otra app («Abrir con»), hasta que lo recoge
    // liblivesketch (takeOpenUri): al arrancar o, con la app ya abierta, cuando se le avisa.
    private static String sOpenUri;

    // El selector de «Exportar a…» está abierto y la app espera su respuesta. Si la actividad
    // vuelve sin ella (por ejemplo, desde Recientes, con el selector aún abierto en otra tarea),
    // se da por cancelado; lo que el selector cree después se borra.
    private static boolean sSaveWaiting;

    // Los documentos de otras apps que tiene abiertos liblivesketch (openDocument), por su
    // descriptor. Se cierran desde aquí (closeDocument) para que esa app sepa que se terminó de
    // escribir (por ejemplo, para subirlo a la nube).
    private static final HashMap<Integer, ParcelFileDescriptor> sDocuments = new HashMap<>();

    // El SDK de NDI para Android usa el servicio NSD del sistema para el descubrimiento y
    // exige que exista una instancia de NsdManager mientras haya senders activos.
    private NsdManager mNsdManager;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "livesketch" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // SDL lo pasaría como archivo soltado con solo la ruta de la URI, que no sirve para
        // abrirlo, y antes de que la app pueda recibirlo: lo recoge la app al arrancar. Solo si
        // se acaba de pedir: al volver a crear la actividad, o al abrirla desde Recientes, llega
        // el mismo intent, y ese proyecto ya se abrió (y quedó en Proyectos).
        Intent intent = getIntent();
        String open = takeViewIntent(intent);
        boolean fromHistory = (intent.getFlags() & Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0;
        if (open != null && savedInstanceState == null && !fromHistory) {
            synchronized (MainActivity.class) {
                sOpenUri = open;
            }
        }

        super.onCreate(savedInstanceState);

        mNsdManager = (NsdManager) getSystemService(Context.NSD_SERVICE);

        // Entrega cada muestra del lápiz en cuanto llega, sin agruparlas por frame:
        // trazos más suaves y con menos latencia (Android 11+).
        if (Build.VERSION.SDK_INT >= 30 && mSurface != null) {
            mSurface.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_POINTER);
        }
    }

    // Con la app ya abierta (solo hay una), otro «Abrir con» llega aquí: se le avisa a la app,
    // que lo recoge con takeOpenUri.
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String open = takeViewIntent(intent);
        if (open != null && !mBrokenLibraries) {
            synchronized (MainActivity.class) {
                sOpenUri = open;
            }
            nativeOpenRequested();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        boolean abandoned;
        synchronized (MainActivity.class) {
            abandoned = sSaveWaiting;
            sSaveWaiting = false;
        }
        if (abandoned && !mBrokenLibraries) {
            nativeSaveFileChosen(null, null);
        }
    }

    // Lo que se pide abrir con un intent VIEW: la URI de su documento (o la ruta, si es un
    // archivo), que se le quita al intent. Null si no es eso.
    private static String takeViewIntent(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction())) {
            return null;
        }
        Uri uri = intent.getData();
        if (uri == null) {
            return null;
        }
        intent.setData(null);
        if ("file".equals(uri.getScheme())) {
            return uri.getPath();
        }
        return uri.toString();
    }

    /**
     * Lo llama liblivesketch (JNI): el proyecto que se pidió abrir desde otra app, en UTF-8,
     * una sola vez; null si no hay.
     */
    public static synchronized byte[] takeOpenUri() {
        String uri = sOpenUri;
        sOpenUri = null;
        return uri != null ? uri.getBytes(StandardCharsets.UTF_8) : null;
    }

    // Con la app ya abierta, llegó otro proyecto para abrir (takeOpenUri). En liblivesketch
    // (IO/FileChooser.cpp).
    private static native void nativeOpenRequested();

    /**
     * Lo llama liblivesketch (JNI, desde su hilo): elegir dónde crear un archivo llamado
     * `name` (en UTF-8). La respuesta llega a nativeSaveFileChosen. False si no se pudo pedir.
     */
    public static boolean chooseSaveFile(byte[] name) {
        final SDLActivity activity = mSingleton;
        if (activity == null || name == null) {
            return false;
        }
        final String title = new String(name, StandardCharsets.UTF_8);
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                // Con un tipo genérico, el sistema deja el nombre tal cual (con su .lvskt).
                intent.setType("application/octet-stream");
                intent.putExtra(Intent.EXTRA_TITLE, title);
                synchronized (MainActivity.class) {
                    sSaveWaiting = true;
                }
                try {
                    activity.startActivityForResult(intent, REQUEST_SAVE_FILE);
                } catch (ActivityNotFoundException e) {
                    Log.e(TAG, "No hay selector de documentos", e);
                    synchronized (MainActivity.class) {
                        sSaveWaiting = false;
                    }
                    nativeSaveFileChosen(null, "este dispositivo no tiene un selector de documentos");
                }
            }
        });
        return true;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQUEST_SAVE_FILE) {
            Uri uri = (resultCode == RESULT_OK && data != null) ? data.getData() : null;
            boolean waiting;
            synchronized (MainActivity.class) {
                waiting = sSaveWaiting;
                sSaveWaiting = false;
            }
            boolean handled = waiting && !mBrokenLibraries &&
                              nativeSaveFileChosen(uri != null ? uri.toString() : null, null);
            // La app ya no lo esperaba: el archivo que creó el selector se quedaría vacío.
            if (!handled && uri != null) {
                deleteDocument(getContentResolver(), uri);
                Toast.makeText(this, "No se exportó el proyecto: vuelve a intentarlo", Toast.LENGTH_LONG).show();
            }
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    // Respuesta de chooseSaveFile: la URI del documento creado, o null si se canceló (o si
    // falló, con el motivo en `error`). Devuelve false si la app no la esperaba. En
    // liblivesketch (IO/FileChooser.cpp).
    private static native boolean nativeSaveFileChosen(String uri, String error);

    /**
     * Lo llama liblivesketch (JNI, desde cualquier hilo): abre el documento `uri` (en UTF-8)
     * con el modo de ContentResolver `mode` ("r" para leer, "wt" para escribirlo de nuevo).
     * Devuelve su descriptor, que sigue siendo de aquí hasta closeDocument, o -1 si no se
     * pudo (por ejemplo, si ya no hay permiso para leerlo). A diferencia de la apertura de
     * SDL, no deja una excepción de Java pendiente.
     */
    public static int openDocument(byte[] uri, String mode) {
        final SDLActivity activity = mSingleton;
        if (activity == null || uri == null || mode == null) {
            return -1;
        }
        String text = new String(uri, StandardCharsets.UTF_8);
        try {
            ParcelFileDescriptor document;
            try {
                document = activity.getContentResolver().openFileDescriptor(Uri.parse(text), mode);
            } catch (IllegalArgumentException | UnsupportedOperationException | FileNotFoundException e) {
                // Hay apps que no admiten "wt". Lo que se escribe es un documento recién creado
                // (vacío), así que da igual que "w" no lo vacíe antes.
                if (!"wt".equals(mode)) {
                    throw e;
                }
                document = activity.getContentResolver().openFileDescriptor(Uri.parse(text), "w");
            }
            if (document == null) {
                return -1;
            }
            int fd = document.getFd();
            synchronized (sDocuments) {
                sDocuments.put(fd, document);
            }
            return fd;
        } catch (Exception e) {
            Log.w(TAG, "No se pudo abrir " + text, e);
            return -1;
        }
    }

    /**
     * Lo llama liblivesketch (JNI): cierra un documento de openDocument. Con `failed`, la app
     * que lo da sabe que no se terminó de escribir. False si no se pudo cerrar bien.
     */
    public static boolean closeDocument(int fd, boolean failed) {
        ParcelFileDescriptor document;
        synchronized (sDocuments) {
            document = sDocuments.remove(fd);
        }
        if (document == null) {
            return false;
        }
        try {
            if (failed) {
                document.closeWithError("no se terminó de escribir");
            } else {
                document.close();
            }
            return true;
        } catch (IOException e) {
            Log.w(TAG, "No se pudo cerrar un documento", e);
            return false;
        }
    }

    /**
     * Lo llama liblivesketch (JNI, desde cualquier hilo): borra el documento `uri` (en UTF-8),
     * el que creó el selector para una exportación que falló. False si no se pudo.
     */
    public static boolean removeDocument(byte[] uri) {
        final SDLActivity activity = mSingleton;
        if (activity == null || uri == null) {
            return false;
        }
        return deleteDocument(activity.getContentResolver(), Uri.parse(new String(uri, StandardCharsets.UTF_8)));
    }

    private static boolean deleteDocument(ContentResolver resolver, Uri uri) {
        try {
            return DocumentsContract.deleteDocument(resolver, uri);
        } catch (Exception e) {
            Log.w(TAG, "No se pudo borrar " + uri, e);
            return false;
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
