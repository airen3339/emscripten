// LLVM => JavaScript compiler, main entry point

try {
  // On SpiderMonkey, prepare a large amount of GC space
  gcparam('maxBytes', 1024*1024*1024);
} catch(e) {}

// Prep - allow this to run in both SpiderMonkey and V8
if (!this['load']) {
  load = function(f) { eval(snarf(f)) };
}
if (!this['read']) {
  read = function(f) { snarf(f) };
}
if (!this['arguments']) {
  arguments = scriptArgs;
}

// Basic utilities

load('utility.js');

// Load settings, can be overridden by commandline

load('settings.js');

var settings_file = arguments[0];
var ll_file = arguments[1];

var settings = JSON.parse(read(settings_file));
for (setting in settings) {
  this[setting] = settings[setting];
}

var CONSTANTS = { 'QUANTUM_SIZE': QUANTUM_SIZE };

if (CORRECT_SIGNS >= 2) {
  CORRECT_SIGNS_LINES = set(CORRECT_SIGNS_LINES); // for fast checking
}
if (CORRECT_OVERFLOWS >= 2) {
  CORRECT_OVERFLOWS_LINES = set(CORRECT_OVERFLOWS_LINES); // for fast checking
}
if (CORRECT_ROUNDINGS >= 2) {
  CORRECT_ROUNDINGS_LINES = set(CORRECT_ROUNDINGS_LINES); // for fast checking
}
if (SAFE_HEAP >= 2) {
  SAFE_HEAP_LINES = set(SAFE_HEAP_LINES); // for fast checking
}

EXPORTED_FUNCTIONS = set(EXPORTED_FUNCTIONS);
EXPORTED_GLOBALS = set(EXPORTED_GLOBALS);

// Settings sanity checks

assert(!(USE_TYPED_ARRAYS === 2 && QUANTUM_SIZE !== 4), 'For USE_TYPED_ARRAYS == 2, must have normal QUANTUM_SIZE of 4');

// Load compiler code

load('framework.js');
load('modules.js');
load('parseTools.js');
load('intertyper.js');
load('analyzer.js');
load('jsifier.js');
eval(processMacros(preprocess(read('runtime.js'))));

//===============================
// Main
//===============================

// Read llvm

var lines;

if( ll_file && ll_file.length != 0 )
{
	var raw = read(ll_file);
	if (FAKE_X86_FP80)
		raw = raw.replace(/x86_fp80/g, 'double');
		
	lines = raw.split('\n');
}
else
{
	lines = [];
	
	// Use stdin
	while( true )
	{
		var line = readline();
		
		if( !line )
			break;
		
		if (FAKE_X86_FP80)
			line = line.replace(/x86_fp80/g, 'double');

		lines.push(line);
	}
	
}

// Do it

//dprint(JSON.stringify(C_DEFINES));

JSify(analyzer(intertyper(lines)));

