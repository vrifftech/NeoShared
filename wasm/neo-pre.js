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

  async function importHostFiles(options) {
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
  }

  function completeOpenFilesRequest(requestId, paths, error) {
    // The wx DOM port enters C++ menu/control handlers through a synchronous
    // ccall. Complete in a later browser task so the original wx event has
    // fully unwound before entering WebAssembly again.
    window.setTimeout(function() {
      try {
        if (!Module.ccall) throw new Error('Emscripten ccall is unavailable.');
        Module.ccall(
          'neo_browser_open_files_completed',
          null,
          ['number', 'string', 'string'],
          [requestId, (paths || []).join('\n'), error || '']);
      } catch (callbackError) {
        console.error('[NeoTools] Browser file completion callback failed:', callbackError);
      }
    }, 0);
  }


  var activeDownloadEntries = [];

  function ensureDownloadPanel() {
    var panel = document.getElementById('neo-download-panel');
    if (panel) return panel;

    panel = document.createElement('div');
    panel.id = 'neo-download-panel';
    panel.setAttribute('role', 'status');
    panel.setAttribute('aria-live', 'polite');
    panel.style.position = 'fixed';
    panel.style.right = '12px';
    panel.style.bottom = '12px';
    panel.style.zIndex = '2147483647';
    panel.style.display = 'flex';
    panel.style.flexDirection = 'column';
    panel.style.gap = '8px';
    panel.style.width = 'min(440px, calc(100vw - 24px))';
    panel.style.pointerEvents = 'none';
    document.body.appendChild(panel);
    return panel;
  }

  function removeDownloadItem(item, url) {
    if (item && item.parentNode) item.parentNode.removeChild(item);
    for (var index = activeDownloadEntries.length - 1; index >= 0; --index) {
      if (activeDownloadEntries[index].item === item || activeDownloadEntries[index].url === url) {
        activeDownloadEntries.splice(index, 1);
      }
    }
    try { URL.revokeObjectURL(url); } catch (_) {}
  }

  function presentBrowserDownload(bytes, name) {
    var blob = new Blob([bytes], { type: 'application/octet-stream' });
    var url = URL.createObjectURL(blob);

    var panel = ensureDownloadPanel();
    var item = document.createElement('div');
    item.style.display = 'flex';
    item.style.alignItems = 'center';
    item.style.gap = '8px';
    item.style.padding = '9px 10px';
    item.style.border = '1px solid #596473';
    item.style.borderRadius = '7px';
    item.style.background = '#252b33';
    item.style.color = '#eef2f6';
    item.style.boxShadow = '0 6px 22px rgba(0,0,0,.35)';
    item.style.pointerEvents = 'auto';

    var label = document.createElement('span');
    label.textContent = 'Download ready:';
    label.style.flex = '0 0 auto';
    label.style.fontSize = '13px';

    var anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = name;
    anchor.textContent = name;
    anchor.title = 'Download ' + name;
    anchor.style.flex = '1 1 auto';
    anchor.style.minWidth = '0';
    anchor.style.overflow = 'hidden';
    anchor.style.textOverflow = 'ellipsis';
    anchor.style.whiteSpace = 'nowrap';
    anchor.style.color = '#9dc8ff';
    anchor.style.fontWeight = '600';

    var close = document.createElement('button');
    close.type = 'button';
    close.textContent = '×';
    close.title = 'Dismiss download';
    close.setAttribute('aria-label', 'Dismiss download');
    close.style.flex = '0 0 auto';
    close.style.width = '26px';
    close.style.height = '26px';
    close.style.padding = '0';
    close.style.border = '1px solid #66717f';
    close.style.borderRadius = '5px';
    close.style.background = '#343c47';
    close.style.color = '#eef2f6';
    close.style.cursor = 'pointer';
    close.addEventListener('click', function() { removeDownloadItem(item, url); });

    item.appendChild(label);
    item.appendChild(anchor);
    item.appendChild(close);
    panel.appendChild(item);
    activeDownloadEntries.push({ item: item, url: url });
    while (activeDownloadEntries.length > 6) {
      var oldest = activeDownloadEntries[0];
      removeDownloadItem(oldest.item, oldest.url);
    }

    // Attempt the ordinary immediate download while transient activation is
    // still present. The visible link remains even if the browser blocks the
    // synthetic click, so the user always has a real click target to retry.
    var mayAutoDownload = !navigator.userActivation || navigator.userActivation.isActive;
    if (mayAutoDownload) {
      try { anchor.click(); } catch (error) {
        console.warn('[NeoTools] Automatic download was blocked; use the visible download link.', error);
      }
    }
    return true;
  }

  Module.neoToolsBrowserFiles = {
    requestOpenFiles: function(requestId, options) {
      try {
        // Calling the async function starts chooseHostFiles() immediately,
        // while this invocation still has the user's click activation. Only the
        // completion is deferred.
        var importPromise = importHostFiles(options);
        Promise.resolve(importPromise).then(
          function(paths) { completeOpenFilesRequest(requestId, paths, ''); },
          function(error) {
            var message = error && error.message ? error.message : String(error || 'Unknown browser file error.');
            console.error('[NeoTools] Browser file picker failed:', error);
            completeOpenFilesRequest(requestId, [], message);
          });
        return true;
      } catch (error) {
        var message = error && error.message ? error.message : String(error || 'Unknown browser file error.');
        console.error('[NeoTools] Browser file picker request failed:', error);
        completeOpenFilesRequest(requestId, [], message);
        return true;
      }
    },

    chooseOpenFiles: function(options) {
      return importHostFiles(options);
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
      return presentBrowserDownload(bytes, name);
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
  window.addEventListener('pagehide', function() {
    while (activeDownloadEntries.length) {
      var entry = activeDownloadEntries.pop();
      try { URL.revokeObjectURL(entry.url); } catch (_) {}
    }
    flushSettings();
  });
  document.addEventListener('visibilitychange', function() {
    if (document.visibilityState === 'hidden') flushSettings();
  });
  window.setInterval(flushSettings, 30000);
})();
