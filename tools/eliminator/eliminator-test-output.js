function f() {
  
  
  
  
  HEAP[123] = (GLOB[1] + 1) / 2;
}
function g(a1, a2) {
  var a = 1;
  
  var c = a * 2 - 1;
  
  a = c;
  foo(c);
  
  foo(2);
  for (var i = 0; i < 5; i++) {
    var q = {
      a: 1
    } + [ 2, 3 ];
  }
  for (var iterator in SOME_GLOBAL) {
    quux(iterator);
  }
  var $0 = HEAP[5];
  MAYBE_HEAP[myglobal] = 123;
  
  if ($0 < 0) {
    __label__ = 1;
  } else {
    __label__ = 2;
  }
  var sadijn = new asd;
  sadijn2 = "qwe%sert";
  this.Module || (this.Module = {});
  var obj = {
    "quoted": 1,
    "doublequoted": 2,
    unquoted: 3,
    4: 5
  };
}
function h() {
  var out;
  bar(hello);
  var hello = 5;
  if (0) {
    var sb1 = 21;
  }
  out = sb1;
  if (0) {
    var sb2 = 23;
  } else {
    out = sb2;
  }
  if (0) {
    out = sb3;
  } else {
    var sb3 = 23;
  }
  for (var it = 0; it < 5; it++) {
    x = y ? x + 1 : 7;
    var x = -5;
  }
  
  if (1) {
    otherGlob = glob;
    breakMe();
  }
  var oneUse2 = glob2;
  while (1) {
    otherGlob2 = oneUse2;
    breakMe();
  }
  return out;
}
function strtok_part(b, j, f) {
  var a;
  for (;;) {
    h = a == 13 ? h : 0;
    a = HEAP[d + h];
    if (a == g != 0) break;
    var h = h + 1;
    if (a != 0) a = 13;
  }
}
function py() {
  
  
  var $7 = HEAP[HEAP[__PyThreadState_Current] + 12] + 1;
  var $8 = HEAP[__PyThreadState_Current] + 12;
  HEAP[$8] = $7;
}
function otherPy() {
  var $4 = HEAP[__PyThreadState_Current];
  var $5 = $4 + 12;
  var $7 = HEAP[$5] + 1;
  var $8 = $4 + 12;
  HEAP[$8] = $7;
}
var anon = (function(x) {
  var $4 = HEAP[__PyThreadState_Current];
  var $5 = $4 + 12;
  var $7 = HEAP[$5] + 1;
  var $8 = $4 + 12;
  HEAP[$8] = $7;
});
function r($0) {
  HEAP[$0 + 7] = 107;
}
