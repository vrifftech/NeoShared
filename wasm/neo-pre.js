// NeoTools browser bootstrap. This runs after the shell creates Module and
// before the generated Emscripten runtime starts.
(function() {
  'use strict';
  if (typeof Module === 'undefined') return;

  var persistentRoot = '/home/web_user/.config/neotools';
  var syncing = false;
  var browserFileSequence = 0;

  function safeBrowserFileName(name, fallback) {
    var value = String(name || '').replace(/[\\/\u0000-\u001f\u007f]/g, '_').trim();
    if (!value || value === '.' || value === '..') value = fallback || 'download.bin';
    return value;
  }

  function uniqueBrowserDirectory(kind) {
    browserFileSequence += 1;
    var directory = '/tmp/neotools-' + kind + '/' +
      Date.now().toString(36) + '-' + browserFileSequence.toString(36);
    FS.mkdirTree(directory);
    return directory;
  }

  function chooseHostFiles(options) {
    options = options || {};
    return new Promise(function(resolve) {
      var input = document.createElement('input');
      input.type = 'file';
      input.multiple = !!options.multiple;
      if (options.accept) input.accept = options.accept;
      input.style.position = 'fixed';
      input.style.left = '-10000px';
      input.style.top = '-10000px';
      input.setAttribute('aria-hidden', 'true');

      var settled = false;
      var focusTimer = 0;
      function onWindowFocus() {
        focusTimer = window.setTimeout(function() {
          if (!settled && (!input.files || input.files.length === 0)) finish([]);
        }, 350);
      }
      function finish(files) {
        if (settled) return;
        settled = true;
        if (focusTimer) window.clearTimeout(focusTimer);
        window.removeEventListener('focus', onWindowFocus);
        if (input.parentNode) input.parentNode.removeChild(input);
        resolve(files || []);
      }

      input.addEventListener('change', function() {
        finish(Array.prototype.slice.call(input.files || []));
      }, { once: true });
      input.addEventListener('cancel', function() { finish([]); }, { once: true });

      // Older browsers don't emit the input cancel event. When focus returns
      // after the native picker closes, allow the change event to arrive first,
      // then treat an empty selection as cancellation.
      window.addEventListener('focus', onWindowFocus);

      document.body.appendChild(input);
      try {
        input.click();
      } catch (error) {
        console.error('[NeoTools] Unable to open the browser file picker:', error);
        finish([]);
      }
    });
  }

  Module.neoToolsBrowserFiles = {
    chooseOpenFiles: async function(options) {
      if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
      var files = await chooseHostFiles(options);
      if (!files.length) return [];

      var directory = uniqueBrowserDirectory('import');
      var used = Object.create(null);
      var paths = [];
      for (var index = 0; index < files.length; ++index) {
        var file = files[index];
        var originalName = safeBrowserFileName(file.name, 'import.bin');
        var candidate = originalName;
        var suffix = 2;
        while (used[candidate]) {
          var dot = originalName.lastIndexOf('.');
          var stem = dot > 0 ? originalName.substring(0, dot) : originalName;
          var extension = dot > 0 ? originalName.substring(dot) : '';
          candidate = stem + '-' + suffix + extension;
          suffix += 1;
        }
        used[candidate] = true;

        var bytes = new Uint8Array(await file.arrayBuffer());
        var path = directory + '/' + candidate;
        FS.writeFile(path, bytes);
        paths.push(path);
      }
      return paths;
    },

    chooseSaveFile: function(options) {
      if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
      options = options || {};
      var fallback = safeBrowserFileName(options.defaultFile, 'download.bin');
      var chosen = window.prompt(options.title || 'Download file as', fallback);
      if (chosen === null) return null;
      chosen = safeBrowserFileName(chosen, fallback);
      return uniqueBrowserDirectory('export') + '/' + chosen;
    },

    downloadFile: function(path, downloadName) {
      if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
      var bytes = FS.readFile(path, { encoding: 'binary' });
      var name = safeBrowserFileName(downloadName,
        safeBrowserFileName(String(path || '').split('/').pop(), 'download.bin'));
      var blob = new Blob([bytes], { type: 'application/octet-stream' });
      var url = URL.createObjectURL(blob);
      var anchor = document.createElement('a');
      anchor.href = url;
      anchor.download = name;
      anchor.style.display = 'none';
      document.body.appendChild(anchor);
      anchor.click();
      anchor.remove();
      window.setTimeout(function() { URL.revokeObjectURL(url); }, 1000);
      return true;
    }
  };

  Module.preRun = Module.preRun || [];
  Module.preRun.push(function() {
    if (typeof FS === 'undefined' || typeof IDBFS === 'undefined') return;
    try {
      if (typeof ENV !== 'undefined') ENV.HOME = '/home/web_user';
      FS.mkdirTree(persistentRoot);
      try { FS.mount(IDBFS, {}, persistentRoot); } catch (mountError) {
        // EBUSY means the mount already exists after a soft reload.
        if (!mountError || mountError.errno !== 10) console.warn('[NeoTools] IDBFS mount:', mountError);
      }
      addRunDependency('neotools-idbfs');
      FS.syncfs(true, function(error) {
        if (error) console.warn('[NeoTools] Unable to restore browser settings:', error);
        removeRunDependency('neotools-idbfs');
      });
    } catch (error) {
      console.warn('[NeoTools] Persistent settings are unavailable:', error);
    }
  });

  function flushSettings() {
    if (syncing || typeof FS === 'undefined' || typeof IDBFS === 'undefined') return;
    syncing = true;
    FS.syncfs(false, function(error) {
      syncing = false;
      if (error) console.warn('[NeoTools] Unable to persist browser settings:', error);
    });
  }

  window.neoToolsFlushSettings = flushSettings;
  window.addEventListener('pagehide', flushSettings);
  document.addEventListener('visibilitychange', function() {
    if (document.visibilityState === 'hidden') flushSettings();
  });
  window.setInterval(flushSettings, 30000);
})();
