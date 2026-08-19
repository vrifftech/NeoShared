// NeoTools browser bootstrap. This runs after the shell creates Module and
// before the generated Emscripten runtime starts.
(function() {
  'use strict';
  if (typeof Module === 'undefined') return;

  var persistentRoot = '/home/web_user/.config/neotools';
  var syncing = false;

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
