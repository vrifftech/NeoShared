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

  function completeDownloadRequest(requestId, disposition, error) {
    // As with file-open completion, leave the originating wx DOM event before
    // entering WebAssembly again. The callback updates status or displays an
    // error through wx's normal pending-event pump.
    window.setTimeout(function() {
      try {
        if (!Module.ccall) throw new Error('Emscripten ccall is unavailable.');
        Module.ccall(
          'neo_browser_download_completed',
          null,
          ['number', 'number', 'string'],
          [requestId, disposition, error || '']);
      } catch (callbackError) {
        console.error('[NeoTools] Browser download completion callback failed:', callbackError);
      }
    }, 0);
  }

  function ensureDownloadHost() {
    var host = document.getElementById('neo-download-host');
    if (host) return host;

    host = document.createElement('div');
    host.id = 'neo-download-host';
    host.setAttribute('role', 'status');
    host.setAttribute('aria-live', 'polite');
    host.style.display = 'flex';
    host.style.alignItems = 'center';
    host.style.gap = '6px';
    host.style.maxWidth = 'min(52vw, 720px)';
    host.style.overflowX = 'auto';
    host.style.overflowY = 'hidden';
    host.style.pointerEvents = 'auto';
    host.style.flex = '0 1 auto';

    var browserBar = document.getElementById('neo-browser-bar');
    if (browserBar) {
      var privacy = browserBar.querySelector('.privacy');
      browserBar.insertBefore(host, privacy || null);
      return host;
    }

    // The generated NeoTools shell always has #neo-browser-bar, but keep an
    // obvious fixed fallback for custom embedding pages.
    host.style.position = 'fixed';
    host.style.right = '12px';
    host.style.top = '12px';
    host.style.zIndex = '2147483647';
    host.style.padding = '6px';
    host.style.background = '#20242b';
    host.style.border = '1px solid #596473';
    host.style.borderRadius = '7px';
    document.body.appendChild(host);
    return host;
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

  function queueBrowserDownload(bytes, name, attemptImmediate) {
    // Copy out of the Emscripten heap before creating the Blob. Memory growth
    // may replace the heap buffer after this function returns.
    var copy = new Uint8Array(bytes.length);
    copy.set(bytes);
    var blob = new Blob([copy], { type: 'application/octet-stream' });
    var url = URL.createObjectURL(blob);

    var host = ensureDownloadHost();
    var item = document.createElement('span');
    item.style.display = 'inline-flex';
    item.style.alignItems = 'center';
    item.style.gap = '4px';
    item.style.flex = '0 0 auto';

    var anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = name;
    anchor.textContent = 'Download ' + name;
    anchor.title = 'Download ' + name;
    anchor.setAttribute('data-neo-download-name', name);
    anchor.style.display = 'inline-flex';
    anchor.style.alignItems = 'center';
    anchor.style.minHeight = '26px';
    anchor.style.maxWidth = '360px';
    anchor.style.padding = '3px 9px';
    anchor.style.border = '1px solid #72a7e7';
    anchor.style.borderRadius = '5px';
    anchor.style.background = '#315f91';
    anchor.style.color = '#ffffff';
    anchor.style.fontWeight = '650';
    anchor.style.textDecoration = 'none';
    anchor.style.whiteSpace = 'nowrap';
    anchor.style.overflow = 'hidden';
    anchor.style.textOverflow = 'ellipsis';
    anchor.style.cursor = 'pointer';

    var close = document.createElement('button');
    close.type = 'button';
    close.textContent = '×';
    close.title = 'Dismiss prepared download';
    close.setAttribute('aria-label', 'Dismiss prepared download ' + name);
    close.style.width = '24px';
    close.style.height = '24px';
    close.style.padding = '0';
    close.style.border = '1px solid #66717f';
    close.style.borderRadius = '5px';
    close.style.background = '#343c47';
    close.style.color = '#eef2f6';
    close.style.cursor = 'pointer';
    close.addEventListener('click', function(event) {
      event.preventDefault();
      event.stopPropagation();
      removeDownloadItem(item, url);
    });

    item.appendChild(anchor);
    item.appendChild(close);
    host.appendChild(item);
    activeDownloadEntries.push({ item: item, url: url, anchor: anchor });
    while (activeDownloadEntries.length > 6) {
      var oldest = activeDownloadEntries[0];
      removeDownloadItem(oldest.item, oldest.url);
    }

    // This is only a convenience attempt. The persistent top-bar link is the
    // authoritative fallback and remains available if the browser blocks the
    // synthetic click or consumes transient activation elsewhere.
    if (attemptImmediate) {
      try { anchor.click(); } catch (error) {
        console.warn('[NeoTools] Automatic download was blocked; use the top-bar download link.', error);
      }
    }
    return true;
  }

  function readDownloadBytes(path) {
    if (typeof FS === 'undefined') throw new Error('Emscripten filesystem is unavailable.');
    var bytes = FS.readFile(path, { encoding: 'binary' });
    var copy = new Uint8Array(bytes.length);
    copy.set(bytes);
    return copy;
  }

  function queueDownloadFromPath(path, name, attemptImmediate) {
    return queueBrowserDownload(readDownloadBytes(path), name, attemptImmediate);
  }

  function saveWithFileSystemAccess(requestId, pickerPromise, path, name) {
    Promise.resolve(pickerPromise).then(function(handle) {
      return Promise.resolve(handle.createWritable()).then(function(writable) {
        var bytes = readDownloadBytes(path);
        return Promise.resolve(writable.write(new Blob([bytes], {
          type: 'application/octet-stream'
        }))).then(function() {
          return writable.close();
        });
      });
    }).then(function() {
      completeDownloadRequest(requestId, 1, ''); // Saved
    }).catch(function(error) {
      if (error && error.name === 'AbortError') {
        completeDownloadRequest(requestId, 0, ''); // Cancelled
        return;
      }

      // Permission/user-activation failures are common when a browser doesn't
      // fully support the File System Access API. Preserve the result as a real
      // top-bar download link instead of dropping the user's extracted bytes.
      try {
        queueDownloadFromPath(path, name, false);
        console.warn('[NeoTools] Native Save dialog was unavailable; use the top-bar download link.', error);
        completeDownloadRequest(requestId, 2, ''); // Ready
      } catch (fallbackError) {
        var message = fallbackError && fallbackError.message
          ? fallbackError.message
          : String(fallbackError || error || 'Unknown browser download error.');
        console.error('[NeoTools] Browser save and download fallback failed:', fallbackError);
        completeDownloadRequest(requestId, 0, message);
      }
    });
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

    requestDownloadFile: function(requestId, path, downloadName) {
      var name = safeBrowserFileName(downloadName,
        safeBrowserFileName(String(path || '').split('/').pop(), 'download.bin'));
      try {
        // Invoke the native picker synchronously while the wx button/menu click
        // still carries transient user activation, but do not await it inside
        // the synchronous wx DOM ccall.
        if (typeof window.showSaveFilePicker === 'function') {
          var pickerPromise;
          try {
            pickerPromise = window.showSaveFilePicker({ suggestedName: name });
          } catch (pickerError) {
            queueDownloadFromPath(path, name, false);
            completeDownloadRequest(requestId, 2, '');
            return true;
          }
          saveWithFileSystemAccess(requestId, pickerPromise, path, name);
          return true;
        }

        queueDownloadFromPath(path, name, true);
        completeDownloadRequest(requestId, 2, '');
        return true;
      } catch (error) {
        var message = error && error.message ? error.message : String(error || 'Unknown browser download error.');
        console.error('[NeoTools] Browser download request failed:', error);
        completeDownloadRequest(requestId, 0, message);
        return true;
      }
    },

    // Compatibility path for applications that cannot consume completion yet.
    // It intentionally exposes a persistent top-bar link instead of claiming
    // that an automatic download was accepted by the browser.
    downloadFile: function(path, downloadName) {
      var name = safeBrowserFileName(downloadName,
        safeBrowserFileName(String(path || '').split('/').pop(), 'download.bin'));
      return queueDownloadFromPath(path, name, true);
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
