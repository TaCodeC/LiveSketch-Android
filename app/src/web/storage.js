// Los datos de la versión web en el navegador. La app guarda en /libsdl (la carpeta de
// SDL_GetPrefPath): los proyectos, las preferencias, los pinceles y los tamaños. Esa carpeta
// vive en la memoria de la página; aquí se copia al almacenamiento del navegador (IndexedDB,
// con el formato de IDBFS) y se recupera al cargar la página, antes de que arranque la app.
//
// - Al arrancar: se monta /libsdl y se lee lo guardado (la app espera a que termine).
// - Module.liveSketchPersist(): la app escribió algo (io::persist); se copia un momento
//   después, y varias llamadas seguidas hacen una sola copia.
// - Cada copia lleva solo lo que cambió en esta página desde la anterior y borra lo que esta
//   página borró. (FS.syncfs dejaría IndexedDB igual que la carpeta de esta página, con lo que
//   borraría lo que hubiera guardado otra pestaña; y, como mira la carpeta antes de leer
//   IndexedDB, falla si mientras tanto la app renombra o borra un archivo.)
// - Una pestaña a la vez: cada una tiene su copia de la carpeta, así que la que se quedó atrás
//   guardaría encima de lo que la otra cambió después. La última que se abre avisa a las demás:
//   guardan lo que tengan (la nueva espera a que terminen antes de leer, con un Web Lock) y se
//   tapan con un aviso que deja volver a usarlas (se vuelven a cargar).
// - Al ocultar o cerrar la página: la app guarda ya lo que tenga (liveSketchSuspend) y se
//   empieza a copiar. Copiar a IndexedDB tarda un momento y una página que se cierra no
//   espera, así que lo que aún no está copiado se deja también en localStorage (que se escribe
//   en el acto), si cabe; al volver a cargar la página se recupera de ahí. Si no cabe, el
//   navegador pregunta antes de salir.
// - Si el navegador no deja guardar (una ventana privada, el almacenamiento bloqueado, sin
//   espacio), la app funciona igual pero lo dice (io::storageProblem): lo que guarda se pierde
//   al cerrar la página.
(function () {
  var ROOT = '/libsdl';
  var DELAY_MS = 250;             // de la última escritura a la copia
  var LOAD_TIMEOUT_MS = 10000;    // si el navegador no responde al leer lo guardado, se arranca sin ello
  var LOCK_NAME = 'livesketch-datos';
  var LOCK_TIMEOUT_MS = 30000;    // lo que se espera a que otra pestaña suelte los datos
  var WAIT_NOTICE_MS = 500;       // al cargar, desde cuándo se dice que se espera a otra pestaña
  var STASH_KEY = 'livesketch-sin-copiar';
  var STASH_LIMIT = 3 * 1024 * 1024;   // bytes de archivos en localStorage (en base64 ocupan 4/3)

  var storage = Module['liveSketchStorage'] = {
    problem: '',      // por qué no se guarda (vacío si se guarda)
    mounted: false,   // se leyó lo guardado: lo que cambie se copia
    syncing: false,   // copia en marcha
    pending: false,   // hay cambios que aún no están en ninguna copia empezada
    replaced: false,  // se abrió otra pestaña: esta guarda lo último y se tapa
    frozen: false,    // ya no copia nada (lo de esta pestaña está guardado)
  };
  // Lo que había en la carpeta en la última copia completa: cuándo se miró y cada archivo y
  // carpeta (ruta → {mtime, dir}). IndexedDB tiene eso, o algo más nuevo de otra pestaña.
  var copied = {at: 0, entries: {}};
  var timer = 0;
  var askedPersist = false;
  var copyFailed = false;   // el problema es de una copia que falló (puede arreglarse solo)
  var whenCopied = [];      // a llamar cuando no quede nada por copiar
  var writing = null;       // la transacción de IndexedDB en marcha
  var tab = {id: Math.random().toString(36).slice(2), started: Date.now()};

  function describe(error) {
    var name = error && error.name ? String(error.name) : '';
    if (name === 'QuotaExceededError') {
      return 'el navegador no deja guardar más: no le queda espacio para esta página. ' +
             'Elimina los proyectos que no uses (si quieres conservarlos, descárgalos antes)';
    }
    if (name === 'SecurityError' || name === 'InvalidStateError' || name === 'UnknownError' ||
        typeof indexedDB === 'undefined') {
      return 'el navegador no deja guardar datos a esta página (puede que sea una ventana privada ' +
             'o que tenga bloqueado el almacenamiento de los sitios), así que lo que hagas se ' +
             'pierde al cerrarla';
    }
    var detail = error && error.message ? error.message : (name || String(error));
    return 'el navegador no pudo guardar los cambios (' + detail + ')';
  }

  function report(error, running) {
    console.warn('LiveSketch: no se pudo guardar en el navegador', error);
    if (storage.problem) {
      return;   // el primero explica el resto
    }
    storage.problem = describe(error);
    // Con la app en marcha, que lo diga ya (al arrancar lo mira ella).
    if (running && Module['_liveSketchStorageProblem']) {
      Module['_liveSketchStorageProblem']();
    }
  }

  // Que el navegador no borre estos datos para hacer sitio (Firefox lo pregunta; Chrome lo
  // decide solo). Una vez, cuando ya hay algo guardado.
  function askPersist() {
    if (askedPersist || !navigator.storage || !navigator.storage.persist) {
      return;
    }
    askedPersist = true;
    navigator.storage.persisted().then(function (persisted) {
      if (!persisted) {
        return navigator.storage.persist();
      }
    }).catch(function () {});
  }

  // La carpeta ahora: ruta → {mtime, dir}, con las rutas que usa IDBFS.
  function snapshot() {
    var entries = {};
    var folders = [ROOT];
    while (folders.length) {
      var folder = folders.pop();
      var names = FS.readdir(folder);
      for (var i = 0; i < names.length; ++i) {
        if (names[i] === '.' || names[i] === '..') {
          continue;
        }
        var path = folder + '/' + names[i];
        var stat = FS.lstat(path);
        var dir = FS.isDir(stat.mode);
        entries[path] = {mtime: stat.mtime.getTime(), dir: dir};
        if (dir) {
          folders.push(path);
        }
      }
    }
    return entries;
  }

  // Lo que cambió de `before` (una copia) a `now`: lo nuevo o cambiado (las carpetas antes que
  // lo que tienen dentro) y lo que ya no está (lo de dentro antes que su carpeta). Lo cambiado
  // en el mismo milisegundo en que se miró la copia cuenta como cambiado: pudo ser después.
  function changes(before, now) {
    var put = [];
    var remove = [];
    for (var path in now) {
      var old = before.entries[path];
      if (!old || old.mtime !== now[path].mtime || old.dir !== now[path].dir || now[path].mtime >= before.at) {
        put.push(path);
      }
    }
    for (var gone in before.entries) {
      if (!(gone in now)) {
        remove.push(gone);
      }
    }
    return {put: put.sort(), remove: remove.sort().reverse()};
  }

  // --- Copia en localStorage de lo que aún no está en IndexedDB ---

  function readStash() {
    try {
      var text = localStorage.getItem(STASH_KEY);
      return text ? JSON.parse(text) : null;
    } catch (error) {
      return null;
    }
  }

  // La de esta pestaña (otra pestaña no borra la que dejó una que se cerró).
  function clearStash(any) {
    try {
      var entry = any ? null : readStash();
      if (any || !entry || entry.tab === tab.id) {
        localStorage.removeItem(STASH_KEY);
      }
    } catch (error) {
    }
  }

  function toBase64(bytes) {
    var parts = [];
    for (var i = 0; i < bytes.length; i += 0x8000) {
      parts.push(String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000)));
    }
    return btoa(parts.join(''));
  }

  function fromBase64(text) {
    var binary = atob(text);
    var bytes = new Uint8Array(binary.length);
    for (var i = 0; i < binary.length; ++i) {
      bytes[i] = binary.charCodeAt(i);
    }
    return bytes;
  }

  // Deja en localStorage los archivos que cambiaron desde la última copia completa y los que
  // se borraron. True si cupo todo (o si no había nada).
  function stash() {
    if (!storage.mounted) {
      return false;
    }
    var now = snapshot();
    var change = changes(copied, now);
    var files = change.put.filter(function (path) { return !now[path].dir; });
    var removed = change.remove.filter(function (path) { return !copied.entries[path].dir; });
    if (!files.length && !removed.length) {
      clearStash();
      return true;
    }
    var bytes = 0;
    for (var i = 0; i < files.length; ++i) {
      bytes += FS.stat(files[i]).size;
    }
    if (bytes > STASH_LIMIT) {
      clearStash();
      return false;
    }
    var entry = {tab: tab.id, at: Date.now(), removed: removed, files: []};
    for (var j = 0; j < files.length; ++j) {
      entry.files.push({path: files[j], mtime: now[files[j]].mtime, data: toBase64(FS.readFile(files[j]))});
    }
    try {
      localStorage.setItem(STASH_KEY, JSON.stringify(entry));
      return true;
    } catch (error) {
      clearStash();
      return false;
    }
  }

  // Al cargar: lo que quedó en localStorage pasa a la carpeta (si IndexedDB no tiene ya algo
  // más nuevo) y de ahí a IndexedDB. Después, `done`.
  function restoreStash(done) {
    var entry = storage.frozen ? null : readStash();   // lo recupera la pestaña que se queda
    if (!entry) {
      done();
      return;
    }
    try {
      (entry.removed || []).forEach(function (path) {
        try {
          if (FS.stat(path).mtime.getTime() < entry.at) {
            FS.unlink(path);
          }
        } catch (error) {
        }
      });
      (entry.files || []).forEach(function (file) {
        var current = null;
        try {
          current = FS.stat(file.path);
        } catch (error) {
        }
        if (current && current.mtime.getTime() > file.mtime) {
          return;   // IndexedDB ya tiene una más nueva
        }
        FS.mkdirTree(file.path.substring(0, file.path.lastIndexOf('/')));
        FS.writeFile(file.path, fromBase64(file.data));
        FS.utime(file.path, file.mtime, file.mtime);
      });
    } catch (error) {
      console.warn('LiveSketch: no se pudo recuperar lo que no se copió', error);
      clearStash(true);
      done();
      return;
    }
    copyToDB(function (error) {
      if (error) {
        storage.pending = true;   // se vuelve a intentar con la próxima copia
      } else {
        clearStash(true);
      }
      done();
    });
  }

  // --- Copia a IndexedDB ---

  // Una transacción que borra `remove` y escribe `put`, leídos de la carpeta en el acto. Sin
  // el permiso (otra pestaña se lo quitó) ya no escribe: lo de la otra es más nuevo.
  function write(db, put, remove, done) {
    if (!put.length && !remove.length) {
      done(null);
      return;
    }
    if (lock.supported && !lock.held) {
      done(new Error('otra pestaña tiene los datos'));
      return;
    }
    var failed = null;
    var finished = false;
    function finish(error) {
      if (!finished) {
        finished = true;
        if (writing === transaction) {
          writing = null;
        }
        done(error);
      }
    }
    function fail(error) {
      failed = failed || error;
    }
    var transaction;
    try {
      transaction = db.transaction([IDBFS.DB_STORE_NAME], 'readwrite');
    } catch (error) {
      finish(error);
      return;
    }
    writing = transaction;
    var store = transaction.objectStore(IDBFS.DB_STORE_NAME);
    transaction.oncomplete = function () {
      finish(failed);
    };
    // Se deshace entera (por ejemplo, si no hay espacio).
    transaction.onabort = function (event) {
      finish(failed || transaction.error || (event.target && event.target.error) || new Error('copia cancelada'));
    };
    remove.forEach(function (path) {
      IDBFS.removeRemoteEntry(store, path, function (error) {
        if (error) {
          fail(error);
        }
      });
    });
    put.forEach(function (path) {
      IDBFS.loadLocalEntry(path, function (error, entry) {
        if (error) {
          fail(error);
          return;
        }
        IDBFS.storeRemoteEntry(store, path, entry, function (error) {
          if (error) {
            fail(error);
          }
        });
      });
    });
  }

  // Lleva a IndexedDB lo que cambió desde la última copia. Primero borra lo borrado, en su
  // propia transacción: si lo nuevo no cabe, al menos se libera el sitio de lo que se eliminó.
  // Lo que se escribe se decide mirando la carpeta justo antes (mientras se borraba, la app pudo
  // cambiar algo).
  function copyToDB(done) {
    IDBFS.getDB(ROOT, function (error, db) {
      if (error) {
        done(error);
        return;
      }
      var first;
      try {
        first = changes(copied, snapshot());
      } catch (e) {
        done(e);
        return;
      }
      write(db, [], first.remove, function (error) {
        if (error) {
          done(error);
          return;
        }
        var now;
        var at;
        var change;
        try {
          now = snapshot();
          at = Date.now();
          change = changes(copied, now);
        } catch (e) {
          done(e);
          return;
        }
        write(db, change.put, change.remove, function (error) {
          if (!error) {
            copied = {at: at, entries: now};
          }
          done(error);
        });
      });
    });
  }

  function sync() {
    if (timer) {
      clearTimeout(timer);
      timer = 0;
    }
    if (!storage.mounted || storage.frozen || storage.syncing) {
      return;
    }
    if (!storage.pending) {
      copiesDone();
      return;
    }
    // Solo copia la pestaña que tiene el permiso (la que se quedó atrás ya no lo pide).
    if (lock.supported && !lock.held) {
      if (storage.replaced) {
        copiesDone();
      } else {
        takeLock(sync);
      }
      return;
    }
    storage.syncing = true;
    storage.pending = false;
    copyToDB(function (error) {
      storage.syncing = false;
      if (error) {
        storage.pending = true;   // lo que no se copió va en la próxima copia
        // Si otra pestaña se llevó el permiso a mitad, no es un problema del navegador.
        if (!lock.supported || lock.held) {
          report(error, true);
          copyFailed = true;
        }
      } else {
        // Vuelve a guardar (por ejemplo, después de eliminar proyectos para hacer sitio).
        if (copyFailed) {
          copyFailed = false;
          storage.problem = '';
        }
        askPersist();
        if (!storage.pending) {
          clearStash();   // ya está todo en IndexedDB
        }
      }
      // Lo que cambió mientras tanto.
      if (storage.pending && !error) {
        sync();
      } else {
        copiesDone();
      }
    });
  }

  // No queda nada por copiar (o la copia falló): lo que esperaba a eso, y el permiso se suelta
  // si ya no hace falta.
  function copiesDone() {
    var callbacks = whenCopied;
    whenCopied = [];
    callbacks.forEach(function (callback) { callback(); });
    updateLock();
  }

  Module['liveSketchPersist'] = function () {
    if (!storage.mounted || storage.frozen) {
      return;
    }
    storage.pending = true;
    if (!timer && !storage.syncing) {
      timer = setTimeout(sync, DELAY_MS);
    }
  };

  // La app guarda lo que tenga y se empieza a copiar. Devuelve true si queda algo que se
  // perdería al cerrar la página ahora.
  function suspend() {
    // Tapada por otra pestaña: lo suyo ya está guardado y no cambia.
    if (storage.frozen && !storage.problem) {
      return false;
    }
    var unsaved = false;
    // Mientras carga la página, la app aún no tiene nada que guardar.
    if (Module['_liveSketchSuspend'] && typeof runtimeInitialized !== 'undefined' && runtimeInitialized) {
      try {
        unsaved = Module['_liveSketchSuspend']() !== 0;
      } catch (error) {
        console.error('LiveSketch: no se pudo guardar al salir', error);
      }
    }
    if (storage.frozen) {
      return unsaved;   // no guardaba en el navegador: solo se pregunta antes de cerrar
    }
    sync();
    if (storage.syncing || storage.pending) {
      unsaved = !stash() || unsaved;
    }
    return unsaved;
  }

  // --- Una pestaña a la vez ---
  //
  // El permiso para escribir en IndexedDB es un Web Lock. Lo tiene la pestaña que se ve (puede
  // tener cambios sin guardar) o que aún está copiando; una pestaña oculta ya lo copió todo y lo
  // suelta. La que se abre avisa a las demás (BroadcastChannel) y espera el permiso antes de
  // leer: la que lo tenía guarda lo último, lo copia, se tapa y lo suelta. Una pestaña colgada
  // lo pierde al cabo de un rato (y si aún escribía, eso se deshace).

  var lock = {
    supported: !!(navigator.locks && navigator.locks.request),
    held: false,      // esta pestaña puede escribir
    release: null,    // lo suelta
    waiting: null,    // lo está pidiendo: a quién avisar cuando lo tenga
    cancel: null,     // deja de pedirlo
  };

  // Pide el permiso (si no lo tiene ya) y después llama a `done`. Si mientras tanto se abre otra
  // pestaña, deja de pedirlo y llama a `done` sin él.
  function takeLock(done) {
    if (!lock.supported || lock.held) {
      done();
      return;
    }
    if (lock.waiting) {
      lock.waiting.push(done);
      return;
    }
    lock.waiting = [done];
    var controller = new AbortController();
    var stealTimer = setTimeout(function () {
      controller.abort();
    }, LOCK_TIMEOUT_MS);
    lock.cancel = function () {
      controller.abort();
    };
    // Avisa a quien esperaba, con el permiso o sin él.
    function stopWaiting() {
      clearTimeout(stealTimer);
      lock.cancel = null;
      var callbacks = lock.waiting || [];
      lock.waiting = null;
      callbacks.forEach(function (callback) { callback(); });
    }
    function granted() {
      var held;
      if (!storage.replaced) {
        lock.held = true;
        held = new Promise(function (resolve) {
          lock.release = resolve;
        });
      }
      stopWaiting();
      updateLock();
      return held;   // sin promesa (se abrió otra pestaña mientras tanto), se suelta ya
    }
    // Sin Web Locks en esta página (por ejemplo, en un marco aislado): cada pestaña va por su
    // cuenta, como en un navegador sin ellos.
    function unavailable(error) {
      console.warn('LiveSketch: sin permiso entre pestañas', error);
      lock.supported = false;
      stopWaiting();
    }
    function failed(error) {
      if (lock.held) {
        lost();
      } else if (lock.waiting) {
        unavailable(error);
      }
    }
    try {
      navigator.locks.request(LOCK_NAME, {signal: controller.signal}, granted).catch(function (error) {
        if (lock.held || !lock.waiting || !error || error.name !== 'AbortError') {
          failed(error);
        } else if (storage.replaced) {
          stopWaiting();   // ya no lo necesita, y no se lo quita a la pestaña nueva
        } else {
          console.warn('LiveSketch: otra pestaña no soltó los datos a tiempo; se le quitan');
          navigator.locks.request(LOCK_NAME, {steal: true}, granted).catch(failed);
        }
      });
    } catch (error) {
      unavailable(error);
    }
  }

  // Otra pestaña le quitó el permiso: lo que estaba escribiendo se deshace (la otra ya lee; lo
  // que no llegó a copiarse lo recupera de localStorage, si cupo) y esta se tapa.
  function lost() {
    if (!lock.held) {
      return;
    }
    lock.held = false;
    lock.release = null;
    if (writing) {
      try {
        writing.abort();
      } catch (error) {
      }
    }
    replaced(true);
  }

  function releaseLock() {
    if (lock.held) {
      var release = lock.release;
      lock.held = false;
      lock.release = null;
      if (release) {
        release();
      }
    }
  }

  // Lo tiene mientras haga falta: si se ve o si queda algo por copiar.
  function updateLock() {
    if (!lock.supported || !storage.mounted) {
      return;
    }
    var needed = !storage.frozen && !storage.replaced &&
                 (document.visibilityState !== 'hidden' || storage.syncing || storage.pending);
    if (needed && !lock.held) {
      takeLock(function () {
        if (storage.pending && !timer) {
          sync();
        }
      });
    } else if (!needed && lock.held && !lock.waiting) {
      releaseLock();
    }
  }

  function newer(message) {
    return message.started > tab.started || (message.started === tab.started && message.id > tab.id);
  }

  function noticeShown() {
    var notice = document.getElementById('other-tab');
    return !!notice && !notice.hidden;
  }

  var noticeWatch = null;

  function showReplaced() {
    var notice = document.getElementById('other-tab');
    if (!notice || !document.body) {
      return;
    }
    var button = document.getElementById('other-tab-use');
    // Al final de la página, encima del lienzo.
    function show() {
      document.body.appendChild(notice);
      if (button) {
        button.focus();
      }
    }
    notice.hidden = false;
    if (button) {
      button.onclick = function () {
        location.reload();
      };
    }
    show();
    // Al crear la ventana, SDL esconde todo lo que no es el lienzo: el aviso vuelve a salir.
    if (!noticeWatch && typeof MutationObserver === 'function') {
      noticeWatch = new MutationObserver(function () {
        if (!notice.hidden && notice.parentNode !== document.body) {
          show();
        }
      });
      noticeWatch.observe(document.body, {childList: true});
    }
  }

  function hideReplaced() {
    var notice = document.getElementById('other-tab');
    if (notice) {
      notice.hidden = true;
    }
  }

  // Se abrió LiveSketch en otra pestaña (o, con `stolen`, otra le quitó el permiso): esta
  // guarda lo último, lo copia y suelta el permiso, y se tapa con el aviso.
  function replaced(stolen) {
    if (storage.replaced) {
      return;
    }
    storage.replaced = true;
    if (lock.cancel) {
      lock.cancel();   // si lo estaba pidiendo, ya no
    }
    // Aún cargando (al terminar se tapa) o sin guardar en el navegador: en ese caso sigue
    // usable para que se pueda descargar lo suyo (al cerrarla pregunta antes).
    if (!storage.mounted) {
      storage.frozen = true;
      releaseLock();
      return;
    }
    showReplaced();
    // Sin el permiso no copia: lo suyo se copió antes de soltarlo.
    if (stolen || (lock.supported && !lock.held)) {
      storage.frozen = true;
      releaseLock();
      return;
    }
    whenCopied.push(function () {
      storage.frozen = true;
      releaseLock();
      // Si lo último no se pudo guardar, sigue usable para descargarlo (la app dice qué pasó).
      if (storage.pending && storage.problem) {
        hideReplaced();
      }
    });
    suspend();
  }

  var channel = null;
  try {
    channel = new BroadcastChannel('livesketch');
  } catch (error) {
    channel = null;   // sin BroadcastChannel (o en un archivo local): el permiso basta, pero tarda
  }
  if (channel) {
    channel.onmessage = function (event) {
      var message = event.data || {};
      if (message.type === 'open' && newer(message)) {
        replaced(false);
      }
    };
    channel.postMessage({type: 'open', id: tab.id, started: tab.started});
  }

  // Con el aviso puesto, ni el teclado ni soltar archivos llegan a la app (no cambia nada que no
  // se guarde). El botón del aviso se pulsa con Intro o espacio.
  ['keydown', 'keyup', 'keypress', 'dragover', 'drop'].forEach(function (type) {
    window.addEventListener(type, function (event) {
      if (!noticeShown()) {
        return;
      }
      event.stopImmediatePropagation();
      if (type === 'keydown' && event.target && event.target.id === 'other-tab-use' &&
          (event.key === 'Enter' || event.key === ' ')) {
        event.preventDefault();
        location.reload();
      } else if (type === 'dragover' || type === 'drop') {
        event.preventDefault();   // que el navegador no abra el archivo en lugar de la página
      }
    }, true);
  });

  // --- Arranque ---

  // El texto de la página de carga (si no muestra un error). Devuelve el que había.
  function loadingText(text) {
    var splash = document.getElementById('splash');
    var status = document.getElementById('status');
    if (!status || (splash && splash.classList.contains('error'))) {
      return null;
    }
    var before = status.textContent;
    status.textContent = text;
    return before;
  }

  Module['preRun'] = [].concat(Module['preRun'] || []);
  Module['preRun'].push(function () {
    try {
      FS.mkdir(ROOT);
      FS.mount(IDBFS, {}, ROOT);
    } catch (error) {
      report(error, false);
      return;
    }
    var settled = false;
    var timeout = 0;
    function done(error) {
      if (settled) {
        return;
      }
      settled = true;
      clearTimeout(timeout);
      if (error) {
        report(error, false);
        releaseLock();
        removeRunDependency('livesketch-storage');
        return;
      }
      // Si ya se abrió otra pestaña después de esta, no copia nada y se tapa.
      storage.frozen = storage.replaced;
      storage.mounted = true;
      copied = {at: Date.now(), entries: snapshot()};
      if (storage.frozen) {
        showReplaced();
      }
      restoreStash(function () {
        removeRunDependency('livesketch-storage');
        updateLock();
      });
    }
    addRunDependency('livesketch-storage');
    // Se lee con el permiso: si otra pestaña lo tiene, primero guarda lo suyo.
    var shown = null;
    var waitTimer = setTimeout(function () {
      shown = loadingText('Esperando a que la otra pestaña termine de guardar…');
    }, WAIT_NOTICE_MS);
    takeLock(function () {
      clearTimeout(waitTimer);
      if (shown !== null) {
        loadingText(shown);
      }
      timeout = setTimeout(function () {
        done({name: 'TimeoutError', message: 'el almacenamiento del navegador no responde'});
      }, LOAD_TIMEOUT_MS);
      FS.syncfs(true, done);
    });
  });

  // Ocultar la página (otra pestaña, minimizar, bloquear el móvil) o cerrarla. Al volver a
  // verse, recupera el permiso.
  document.addEventListener('visibilitychange', function () {
    if (document.visibilityState === 'hidden') {
      suspend();
    }
    updateLock();
  });
  window.addEventListener('pagehide', function () {
    suspend();
  });
  // Una página que vuelve de la caché del navegador (atrás y adelante) tiene lo de antes, y
  // otra pestaña pudo cambiarlo mientras tanto: se vuelve a cargar.
  window.addEventListener('pageshow', function (event) {
    if (event.persisted) {
      location.reload();
    }
  });
  // Antes de cerrar o recargar. SDL avisaría a la app de que termina y la pararía aunque al
  // final la página no se cierre: aquí se guarda todo y la app sigue por si se queda. (Este
  // aviso se registra antes que el de SDL, así que llega primero.)
  window.addEventListener('beforeunload', function (event) {
    event.stopImmediatePropagation();
    if (suspend()) {
      event.preventDefault();
      event.returnValue = '';
    }
  });
})();
