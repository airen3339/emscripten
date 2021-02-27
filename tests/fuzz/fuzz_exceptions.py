import json
import os
import random
import subprocess
import sys
import time

'''
Structural fuzz generator.

Like the AFL and Binaryen etc. fuzzers, we start with random bytes as the input.
However, we start with a tree structure of random bytes, something like

[
  1,
  [
    0.42,
    0.501,
    0.17
  ],
  [
    [
    ],
    [
      0.2,
      0.12
    ]
  ]
]

The grammar here is simply

Node = Number or Array
Array = Node^K, K >= 0
Number = [0..1)

Starting from structured random data has the benefit of making it easy to reduce
on the random input. Consider if the structure of the random input gets mapped
to something else with structure, like a source program, then pruning the input
can lead to similar pruned source programs. (In comparison, unstructured random
data allows for truncation easily, but changing bytes earlier can lead to
dramatic differences in the output.)

To get this benefit, the translator of the random structured data must convert
it to the output in a structured manner. That is, one node should be converted
to a corresponding node, and without looking at other nodes as much as possible,
so that if they are altered, that one will not be.

A downside to this approach is that a very large random input may lead to a very
small output, for example, if a huge nested tree of data is consumed in a place
that just wants a bool.
'''


class StructuredRandomData:
    NUM_TOPLEVEL = 5

    # The range of widths.
    MIN_WIDTH = 1
    MAX_WIDTH = 9

    # The range of depths.
    MIN_DEPTH = 2
    MAX_DEPTH = 10

    # The chance to just emit a number instead of a list.
    NUM_PROB = 0.25

    def __init__(self):
        self.root = [self.make_toplevel() for x in range(self.NUM_TOPLEVEL)]

    def make_toplevel(self):
        depth_left = random.randint(self.MIN_DEPTH, self.MAX_DEPTH)
        return self.make_array(0, depth_left)

    def make_array(self, depth, depth_left):
        width = random.randint(self.MIN_WIDTH, self.MAX_WIDTH)
        # When there is almost no depth left, emit fewer things.
        width = min(width, depth_left + 1)
        return [self.make(depth + 1, depth_left - 1) for i in range(width)]

    def make_num(self):
        return random.random()

    def make(self, depth, depth_left):
        if depth_left == 0 or random.random() < self.NUM_PROB:
            return self.make_num()
        return self.make_array(depth, depth_left)


def numify(node):
    if type(node) == list:
        if len(node) == 0:
            return 0
        return numify(node[0])
    return node


def arrayify(node):
    if type(node) != list:
        return [node]
    return node


def indent(code):
    return '\n'.join(['  ' + line for line in code.splitlines() if line])


class Cursor:
    '''
    A cursor over an array, allowing gradual consumption of it. If we run out, we
    return simple values.
    '''
    def __init__(self, array):
        self.array = arrayify(array)
        self.pos = 0

    def get(self):
        if self.pos >= len(self.array):
            return 0
        self.pos += 1
        return self.array[self.pos - 1]

    def get_num(self):
        return numify(self.get())

    def get_array(self):
        return arrayify(self.get())

    def remaining(self):
        return max(0, len(self.array) - self.pos)

    def has_more(self):
        return self.remaining() > 0

    def get_int(self):
        return int(1000 * self.get_num())


def pick(options, value):
    '''
    Given a list of options (weight, choice), and a value in [0, 1) to help pick from
    them, pick one.
    The options can also not have a weight, in which case the weight is 1.
    '''
    # Scale the value by the total weight.
    assert 0 <= value < 1, value
    options = [option if type(option) in (tuple, list) and len(option) == 2 else (1, option) for option in options]
    total = 0
    for weight, choice in options:
        total += weight
    value *= total

    for weight, choice in options:
        if value < weight:
            return choice
        value -= weight

    raise Exception('inconceivable')


class CppTranslator:
    '''
    Translates random structured data into a random C++ program that uses C++
    exceptions.
    '''

    PREAMBLE = '''\
#include <stdio.h> // avoid iostream C++ code, just test libc++abi, not libc++
#include <stdint.h>

extern void refuel();
extern void checkRecursion();
extern bool getBoolean();

struct Class {
  Class();
  ~Class();
};
'''

    SUPPORT = '''\
#include <stdio.h>
#include <stdlib.h>

const int INIIAL_FUEL = 100;

static int fuel = INIIAL_FUEL;

void refuel() {
  fuel = INIIAL_FUEL;
}

void checkRecursion() {
  if (fuel == 0) {
    puts("out of fuel");
    abort();
  }
  fuel--;
}

// TODO random data
static bool boolean = true;

bool getBoolean() {
  // If we are done, exit all loops etc.
  if (fuel == 0) {
    return false;
  }
  fuel--;
  boolean = !boolean;
  return boolean;
}

struct Class {
  Class();
  ~Class();
};

Class::Class() {
  puts("class-instance");
}

Class::~Class() {
  puts("~class-instance");
}
'''

    def __init__(self, data):
        self.toplevel = Cursor(data)
        self.logging_index = 0
        self.try_nesting = 0
        self.loop_nesting = 0
        self.func_names = []

        # The output is a list of strings which will be concatenated when
        # writing.
        self.output = [self.PREAMBLE]
        self.make_functions()

    '''
    Outputs the main file and the support file on the side. Support code is not
    in the main file so that the optimizer cannot see it all.
    '''
    def write(self, main, support):
        with open(main, 'w') as f:
            f.write('\n'.join(self.output))
        with open(support, 'w') as f:
            f.write(self.SUPPORT)

    def make_structs(self):
        array = arrayify(self.toplevel.get())
        # Global mapping of struct name to its array of fields.
        self.structs = {}
        structs = []
        for node in array:
            name = f'Struct{len(structs)}'
            sig = self.get_types(node)
            self.structs[name] = sig
            fields = '\n'.join([f'  {t} f{i};' for i, t in enumerate(sig)])
            structs.append('''\
struct %(name)s {
%(fields)s
};
''' % locals())
        self.output.append('\n'.join(structs))

    def make_functions(self):
        funcs = []
        main = '''\
int main() {
'''
        while self.toplevel.has_more():
            name = f'func_{len(funcs)}'
            body = indent(self.make_statements(self.toplevel.get()))
            self.func_names.append(name)
            funcs.append('''\
void %(name)s() {
%(body)s
}
''' % locals())
            main += '''\
  // %(name)s
  puts("calling %(name)s");
  refuel();
  try {
    %(name)s();
  } catch (...) {
    puts("main caught from %(name)s");
  }
''' % locals()

        main += '''\
  return 0;
}
'''
        funcs.append(main)
        self.output.append('\n'.join(funcs))

    def make_statements(self, node):
        statements = [self.make_statement(n) for n in arrayify(node)]
        return '\n'.join(statements)

    def make_statement(self, node):
        cursor = Cursor(node)
        options = [
          (1,  self.make_nothing),
          (10, self.make_logging),
          (10, self.make_try),
          (10, self.make_if),
          (5,  self.make_loop),
          (5,  self.make_call),
          (5,  self.make_raii),
        ]
        if self.try_nesting:
            options.append((10, self.make_throw))
        else:
            # Only rarely emit throws outside of a try.
            options.append((2, self.make_throw))
        if self.loop_nesting:
            options.append((10, self.make_branch))
        return pick(options, cursor.get_num())(cursor)

    def make_nothing(self, cursor):
        return ''

    def make_logging(self, cursor):
        if cursor.has_more():
            return f'puts("log(-{cursor.get_int()})");'
        self.logging_index += 1
        return f'puts("log({self.logging_index})");'

    def make_throw(self, cursor):
        return f'throw {cursor.get_int()};'

    def make_raii(self, cursor):
        self.logging_index += 1
        return f'Class instance{self.logging_index};'

    def make_try(self, cursor):
        self.try_nesting += 1
        body = indent(self.make_statements(cursor.get_array()))
        self.try_nesting -= 1
        catch_types = ['int32_t', 'int64_t', 'float', 'double', 'Class']
        catches = []

        def add_catch(ty):
          catch = indent(self.make_statements(cursor.get_array()))
          catches.append('''\
} catch (%(ty)s) {
%(catch)s
''' % locals())

        num = cursor.get_num()
        if num < 0.5:
          add_catch(pick(catch_types, num * 2))
        if not catches or num > 0.5:
          add_catch('...')
        catches = '\n'.join(catches)
        return '''\
try {
%(body)s
%(catches)s
}
''' % locals()

    def make_if(self, cursor):
        if_arm = indent(self.make_statements(cursor.get_array()))

        else_ = ''
        if cursor.get_num() >= 0.5:
            else_arm = indent(self.make_statements(cursor.get_array()))
            else_ = '''\
 else {
%(else_arm)s
}''' % locals()

        return '''\
if (getBoolean()) {
%(if_arm)s
}%(else_)s
''' % locals()

    def make_loop(self, cursor):
        self.loop_nesting += 1
        body = indent(self.make_statements(cursor.get_array()))
        self.loop_nesting -= 1

        return '''\
while (getBoolean()) {
%(body)s
}
''' % locals()

    def make_call(self, cursor):
        if not self.func_names:
            return self.make_nothing(cursor)
        return f'{random.choice(self.func_names)}();'

    def make_branch(self, cursor):
        assert self.loop_nesting
        if cursor.get_num() < 0.5:
            return 'break;'
        else:
            return 'continue;'

    def get_types(self, node):
        return [self.get_type(x) for x in arrayify(node)]

    def get_type(self, node):
        if numify(node) < 0.5:
            return 'uint32_t'
        return 'double'



class LLVMTranslator:
    '''
    Translates random structured data into a random LLVM IR uses wasm
    exceptions.
    '''

    PREAMBLE = '''\
; ModuleID = 'a.cpp'
source_filename = "a.cpp"
target datalayout = "e-m:e-p:32:32-i64:64-n32:64-S128"
target triple = "wasm32-unknown-emscripten"

%struct.Class = type { i8 }

@_ZTIi = external constant i8*
@_ZTId = external constant i8*
'''

    START = '''\
define hidden i32 @main() personality i8* bitcast (i32 (...)* @__gxx_wasm_personality_v0 to i8*) {
entry:
  br label %b0
'''

    POSTAMBLE = '''\

normal_exit:
  ret i32 0
}

declare i32 @puts(i8*)

declare %struct.Class* @_ZN5ClassC1Ev(%struct.Class* nonnull returned dereferenceable(1)) unnamed_addr
declare %struct.Class* @_ZN5ClassD1Ev(%struct.Class* nonnull returned dereferenceable(1)) unnamed_addr

declare zeroext i1 @_Z10getBooleanv()

declare i32 @__gxx_wasm_personality_v0(...)
declare i32 @llvm.eh.typeid.for(i8*)

declare i8* @__cxa_begin_catch(i8*)
declare void @__cxa_end_catch()
declare i8* @__cxa_allocate_exception(i32)
declare void @__cxa_throw(i8*, i8*, i8*)

declare i8* @llvm.wasm.get.exception(token)
declare i32 @llvm.wasm.get.ehselector(token)
declare void @llvm.wasm.rethrow()
'''

    SUPPORT = '''\
#include <stdio.h>
#include <stdlib.h>

const int INIIAL_FUEL = 100;

static int fuel = INIIAL_FUEL;

void refuel() {
  fuel = INIIAL_FUEL;
}

// TODO random data
static bool boolean = true;

bool getBoolean() {
  // If we are done, exit all loops etc.
  if (fuel == 0) {
    return false;
  }
  fuel--;
  boolean = !boolean;
  return boolean;
}

struct Class {
  Class();
  ~Class();
};

Class::Class() {
  puts("class-instance");
}

Class::~Class() {
  puts("~class-instance");
}
'''

    def __init__(self, data):
        self.cursors = [Cursor(data)]
        self.global_index = 1
        self.global_defs = ''

        # The output is a list of strings which will be concatenated when
        # writing.
        main = self.make_main()
        self.output = [self.PREAMBLE, self.global_defs, self.START,
                       main, self.POSTAMBLE]

    '''
    Outputs the main file and the support file on the side. Support code is not
    in the main file so that the optimizer cannot see it all.
    '''
    def write(self, main, support):
        with open(main, 'w') as f:
            f.write('\n'.join(self.output))
        with open(support, 'w') as f:
            f.write(self.SUPPORT)

    def get_global_index(self):
        ret = self.global_index
        self.global_index += 1
        return ret

    def cursor(self):
        return self.cursors[-1]

    def push_cursor(self):
        self.cursors.append(Cursor(self.cursor().get_array()))

    def pop_cursor(self):
        self.cursors.pop()

    def pick(self, options):
        return pick(options, self.cursor().get_num())

    def make_main(self):
        return self.make_blocks(start='b0', backs=[], forwards=['normal_exit'])

    def make_blocks(self, start, backs, forwards):
        '''
        Make a block or blocks, starting with a block of name 'start', and with
        given possible backedges and forward edges that are outside of what we
        create in this call.
        '''
        assert forwards, 'There must be a place to fall through to.'

        contents = self.pick([
            self.make_basic_block,
            self.make_loop,
            self.make_if,
            self.make_if_else,
            self.make_invoke,
        ])(backs, forwards)

        return f'''\
{start}:
{contents}'''

    def make_basic_block_contents(self):
        return self.pick([
            self.make_nothing,
            self.make_logging,
            self.make_call,
        ])()

    def make_basic_block(self, backs, forwards):
        contents = self.make_basic_block_contents()
        terminator = self.pick([
            (10, self.make_forward_branch),
            ( 1, self.make_unreachable),
        ])(backs, forwards)

        return contents + '\n' + terminator

    def make_loop(self, backs, forwards):
        start = f'b{self.get_global_index()}'
        end = f'b{self.get_global_index()}'
        self.push_cursor()
        body = self.make_blocks(start, backs + [start], forwards + [end])
        self.pop_cursor()
        call = f'%c{self.get_global_index()}'

        return f'''\
  br label %{start}

;; loop
{body}

{end}:
  ;; loop end
  {call} = call zeroext i1 @_Z10getBooleanv()
  br i1 {call}, label %{start}, label %{self.pick(forwards)}'''

    def make_if(self, backs, forwards):
        call = f'%c{self.get_global_index()}'
        middle = f'b{self.get_global_index()}'
        self.push_cursor()
        body = self.make_blocks(middle, backs, forwards)
        self.pop_cursor()

        return f'''\
  ;; if
  {call} = call zeroext i1 @_Z10getBooleanv()
  br i1 {call}, label %{middle}, label %{self.pick(forwards)}

{body}'''

    def make_if_else(self, backs, forwards):
        call = f'%c{self.get_global_index()}'
        left = f'b{self.get_global_index()}'
        right = f'b{self.get_global_index()}'
        self.push_cursor()
        left_body = self.make_blocks(left, backs, forwards)
        self.pop_cursor()
        self.push_cursor()
        right_body = self.make_blocks(right, backs, forwards)
        self.pop_cursor()

        return f'''\
  ;; if-else
  {call} = call zeroext i1 @_Z10getBooleanv()
  br i1 {call}, label %{left}, label %{right}

{left_body}

{right_body}'''

    def make_invoke(self, backs, forwards):
        call = f'%c{self.get_global_index()}'
        ok = f'b{self.get_global_index()}'
        bad = f'b{self.get_global_index()}'
        self.push_cursor()
        ok_body = self.make_blocks(ok, backs, forwards)
        self.pop_cursor()
        bad_body = self.make_basic_block_contents()

        return f'''\
  {call} = invoke zeroext i1 @_Z10getBooleanv()
           to label %{ok} unwind label %{bad}.dispatch

{bad}.dispatch:
  %catch.{bad}.0 = catchswitch within none [label %{bad}.start] unwind to caller

{bad}.start:
  %catch.{bad}.1 = catchpad within %catch.{bad}.0 [i8* bitcast (i8** @_ZTId to i8*)]
  %catch.{bad}.2 = call i8* @llvm.wasm.get.exception(token %catch.{bad}.1)
  %catch.{bad}.3 = call i32 @llvm.wasm.get.ehselector(token %catch.{bad}.1)
  %catch.{bad}.4 = call i32 @llvm.eh.typeid.for(i8* bitcast (i8** @_ZTId to i8*))
  %catch.{bad}.matches = icmp eq i32 %catch.{bad}.3, %catch.{bad}.4
  br i1 %catch.{bad}.matches, label %{bad}.catch, label %{bad}.rethrow

{bad}.catch:
  %catch.{bad}.5 = call i8* @__cxa_begin_catch(i8* %catch.{bad}.2) [ "funclet"(token %catch.{bad}.1) ]
{bad_body}
  call void @__cxa_end_catch() [ "funclet"(token %catch.{bad}.1) ]
  catchret from %catch.{bad}.1 to label %{ok}

{bad}.rethrow:
  call void @llvm.wasm.rethrow() [ "funclet"(token %catch.{bad}.1) ]
  unreachable

{ok_body}'''

    def make_nothing(self):
        return '  ;; nothing'

    def make_logging(self):
        num = self.get_global_index()
        global_name = f'@.str.{num}'
        num_len = len(str(num))
        full_len = 6 + num_len
        global_def = f'{global_name} = private unnamed_addr constant [{full_len} x i8] c"log({num})\\00", align 1\n'
        self.global_defs += global_def
        logging = f'  %call{num} = call i32 @puts(i8* getelementptr inbounds ([{full_len} x i8], [{full_len} x i8]* {global_name}, i32 0, i32 0))'
        return logging

    def make_call(self):
        num = self.get_global_index()
        return f'''\
  %instance.{num} = alloca %struct.Class, align 1
  %call.{num} = call %struct.Class* @_ZN5ClassC1Ev(%struct.Class* nonnull dereferenceable(1) %instance.{num})'''

    def make_forward_branch(self, backs, forwards):
        return f'  br label %{self.pick(forwards)}'

    def make_unreachable(self, backs, forwards):
        return f'  unreachable'


# Main harness


def known(err, silent):
    if 'Delegate destination should be in scope' in err:
        # https://github.com/emscripten-core/emscripten/issues/13514
        return True
    #if 'Branch destination should be in scope' in err:
    #    # https://github.com/emscripten-core/emscripten/issues/13515
    #    return True
    if 'Active sort region list not finished' in err:
        # https://github.com/emscripten-core/emscripten/issues/13554
        return True
    if 'Allocation failed' in err:
        # Looks related to "Active sort region", testcases fluctuate.
        return True
    if not silent:
        print('unknown compile error')
    return False


def check_testcase(data, silent=True):
    '''
        Checks if a testcase is valid. Returns True if so.
    '''
    # Generate C++
    CppTranslator(data).write(main='a.cpp', support='b.cpp')

    # Compile with emcc, looking for a compilation error.

    # TODO: also compile b.cpp, and remove -c so that we test linking.
    open('a.sh', 'w').write('''\
ulimit -v 500000
./em++ a.cpp b.cpp -s WASM_BIGINT -fwasm-exceptions -O0 -o a.out.js
''')
    try:
        result = subprocess.run(['bash', 'a.sh'],
                                stderr=subprocess.PIPE, text=True, timeout=2)
    except subprocess.TimeoutExpired:
        if not silent:
            print('timeout error')
        return False

    if not silent:
        print(result.stderr)

    if result.returncode != 0:
        # Ignore if the problem is known, and halt here regardless (as we cannot
        # continue to do any more checks).
        return known(result.stderr, silent)

    # Optimize with binaryen
    '''
    debug_env = os.environ.copy()
    debug_env['BINARYEN_PASS_DEBUG'] = '1'
    # read from a.out.wasm and also write to it, so that the execution below
    # runs on optimized code.
    result = subprocess.run(['/home/azakai/Dev/binaryen/bin/wasm-opt',
                             'a.out.wasm', '-o', 'a.out.wasm', '-Os'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=debug_env)

    if not silent:
        print(result.stderr)

    if result.returncode != 0:
        if not silent:
            print('binaryen opt error')
        return False
    '''
    # Compile normally, run normally and with wasm, and compare the results.

    subprocess.check_call(['c++', 'a.cpp', 'b.cpp'])
    normal = subprocess.run(['./a.out'], stdout=subprocess.PIPE, text=True)
    assert normal.returncode == 0
    wasm = subprocess.run([os.path.expanduser('~/.jsvu/v8'),
                           '--experimental-wasm-eh', 'a.out.js'],
                           stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE,
                           text=True)
    if wasm.returncode != 0:
        if not silent:
            print('runtime error')
        return False

    if normal.stdout != wasm.stdout:
        if not silent:
            print('comparison error')
        return False

    return True

'''
TODO: LLVM fuzzing

    result = subprocess.run(['/home/azakai/Dev/sdk/upstream/bin/llc', 'a.ll',
                             '-exception-model=wasm',
                             '-mattr=+exception-handling',
                             '-o', '/dev/null'],
                            stderr=subprocess.PIPE, text=True)

    if not silent:
        print(result.stderr)

    if result.returncode != 0:
        return known(result.stderr, silent)

    return True # XXX
'''

def reduce(data):
    '''
    Given a failing testcase, reduce the input data to create a reduced C++
    testcase.
    '''

    assert not check_testcase(data)

    # The input is structured. The simplest thing is to reduce on it in text
    # form, so that we do not need to have references to nested things etc.
    text = json.dumps(data)
    assert not check_testcase(json.loads(text))
    print(f'[reducing, starting from size {len(text)}]')

    def iteration(text):
        # Find ',' delimiters, and reduce using it it, starting from the start
        # and going to the end (doing this on the text lets us handle all
        # nested structure in a single loop)
        print(f'[reduction iteration begins]')

        def find_delimiter_at_same_scope(text, delimiters, i):
            '''
            Given a reference to the first comma here:
                [1,2,3]
                  ^
            We return a reference to the next one, or to a ] if there is none.
            This handles scoping, that is,
                [1,[5,6],3]
                  ^
            That will return the comma before the '3'.
            '''
            nesting = 0
            while True:
                curr = text[i]
                if curr in delimiters and nesting == 0:
                    return i
                elif curr == '[':
                    nesting += 1
                elif curr == ']':
                    nesting -= 1
                i += 1

        # Reduce starting from commas.
        i = 0
        while True:
            i = text.find(',', i)
            if i < 0:
                break
            # Look for the ], which might allow us to reduce all the tail of the
            # current array. Often the tails are ignored, so this is a big
            # speedup potentially.
            j = find_delimiter_at_same_scope(text, ']', i + 1)

            # We now have something like
            #   ,...,
            #   i   j
            # or
            #   ,...]
            #   i   j
            # And we can try a reduction by removing up to j.
            new_text = text[:i] + text[j:]
            if not check_testcase(json.loads(new_text)):
                # This is a successful reduction!
                text = new_text
                print(f'[reduced (large) to {len(text)}]')
                # Note that i can stay where it is.
                continue

            # The reduction failed. Try a smaller reduction, not all the way
            # to the end of the tail.
            j = find_delimiter_at_same_scope(text, ',]', i + 1)
            if text[j] == ',':
                new_text = text[:i] + text[j:]
                if not check_testcase(json.loads(new_text)):
                    text = new_text
                    print(f'[reduced (small) to {len(text)}]')
                    continue
            i += 1

        # Reduce starting from open braces. This handles removing the very first
        # element.
        i = 0
        while True:
            i = text.find('[', i)
            if i < 0:
                break
            j = find_delimiter_at_same_scope(text, ',]', i + 1)
            if text[j] == ',':
                # [..,  =>  [
                new_text = text[:i + 1] + text[j + 1:]
            else:
                # [..]  =>  []
                new_text = text[:i + 1] + text[j:]
            if not check_testcase(json.loads(new_text)):
                text = new_text
                print(f'[reduced (open) to {len(text)}]')
            i += 1

        # Reduce a singleton parent to a child,
        #  [[x]]  =>  [x]
        i = 0
        while True:
            i = text.find('[', i)
            if i < 0:
                break
            if text[i + 1] != '[':
                i += 1
                continue
            j = find_delimiter_at_same_scope(text, ']', i + 2)
            if text[j + 1] != ']':
                i += 1
                continue
            # We now have
            # [[..]]
            # i   j
            new_text = text[:i] + text[i + 1:j + 1] + text[j + 2:]
            if not check_testcase(json.loads(new_text)):
                text = new_text
                print(f'[reduced (singleton) to {len(text)}]')
                continue
            i += 1

        return text

    # Main loop: do iterations while we are still reducing.
    while True:
        reduced = iteration(text)
        if reduced == text:
            break
        text = reduced

    # Run and verify the final reduction.
    assert not check_testcase(json.loads(text))
    print('[reduction complete, reduced testcase written out]')


def main():
    given_seed = None
    if len(sys.argv) == 2:
        given_seed = int(sys.argv[1])

    total = 0
    seed = time.time() * os.getpid()
    random.seed(seed)

    while 1:
        seed = random.randint(0, 1 << 64)
        if given_seed is not None:
            seed = given_seed
        random.seed(seed)
        print(f'[iteration {total} (seed = {seed})]')
        total += 1

        # Generate a testcase.
        data = StructuredRandomData().root

        # Test it.
        if not check_testcase(data, silent=False):
            print('[testcase failed]')
            reduce(data)
            sys.exit(1)

        if given_seed is not None:
            break

main()
