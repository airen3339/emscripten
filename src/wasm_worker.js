// N.B. The contents of this file are duplicated in src/library_wasm_worker.js
// in variable "_wasmWorkerBlobUrl" (where the contents are pre-minified) If
// doing any changes to this file, be sure to update the contents there too.

// FIXME: Should we also support the WASM_WORKERS == 2 build mode on Node.js?
#if ENVIRONMENT_MAY_BE_NODE
// Node.js support
var ENVIRONMENT_IS_NODE = typeof process == 'object' && typeof process.versions == 'object' && typeof process.versions.node == 'string';
if (ENVIRONMENT_IS_NODE) {
	// Create as web-worker-like an environment as we can.

	var nodeWorkerThreads = require('worker_threads');

	var parentPort = nodeWorkerThreads.parentPort;

	// We only need to listen to the bootstrap message, so use `.once` here.
	parentPort.once('message', (data) => onmessage({ data: data }));

	var fs = require('fs');

	Object.assign(global, {
		self: global,
		require: require,
		location: {
			href: __filename
		},
		Worker: nodeWorkerThreads.Worker,
		importScripts: function(f) {
			(0, eval)(fs.readFileSync(f, 'utf8'));
		},
		postMessage: function(msg) {
			parentPort.postMessage(msg);
		},
		addEventListener: function(type, listener) {
			parentPort.addListener(type, listener);
		},
		removeEventListener: function(type, listener) {
			parentPort.removeListener(type, listener);
		},
		performance: global.performance || {
			now: function() {
				return Date.now();
			}
		},
	});
}
#endif // ENVIRONMENT_MAY_BE_NODE

onmessage = function(d) {
	// The first message sent to the Worker is always the bootstrap message.
	// Drop this message listener, it served its purpose of bootstrapping
	// the Wasm Module load, and is no longer needed. Let user code register
	// any desired message handlers from now on.
	onmessage = null;
	d = d.data;
#if !MODULARIZE
	self.{{{ EXPORT_NAME }}} = d;
#endif
#if !MINIMAL_RUNTIME
	d['instantiateWasm'] = (info, receiveInstance) => { var instance = new WebAssembly.Instance(d['wasm'], info); receiveInstance(instance, d['wasm']); return instance.exports; }
#endif
	importScripts(d.js);
#if MODULARIZE
	{{{ EXPORT_NAME }}}(d);
#endif
	// Drop now unneeded references to from the Module object in this Worker,
	// these are not needed anymore.
	d.wasm = d.mem = d.js = 0;
}
