// LLVM assembly => internal intermediate representation, which is ready
// to be processed by the later stages.

var tokenizer; // TODO: Clean this up/out

//! @param parseFunctions We parse functions only on later passes, since we do not
//!                       want to parse all of them at once, and have all their
//!                       lines and data in memory at the same time.
function intertyper(data, parseFunctions, baseLineNum) {
  //parseFunctions = true; // Uncomment to do all parsing in a single big RAM-heavy pass. Faster, if you have the RAM
  baseLineNum = baseLineNum || 0;

  // Substrate

  if (LLVM_STYLE === null) {
    // new = clang on 2.8, old = llvm-gcc anywhere or clang on 2.7
    LLVM_STYLE = (data.indexOf('<label>') == -1 && data.indexOf('entry:') != -1) ? 'old' : 'new';
    //dprint('LLVM_STYLE: ' + LLVM_STYLE);
  }

  // If the source contains debug info as LLVM metadata, process that out (and save the debugging info for later)
  for (var i = data.length-1; i >= 0; i--) {
    if (/^!\d+ = metadata .*/.exec(data[i])) {
      data = Debugging.processMetadata(data);
      //print(data.join('\n'));
      //dprint(JSON.stringify(Debugging));
      break;
    }
  }

  substrate = new Substrate('Intertyper');

  // Line splitter.
  substrate.addActor('LineSplitter', {
    processItem: function(item) {
      var lines = item.llvmLines;
      var ret = [];
      var inContinual = false;
      var inFunction = false;
      var currFunctionLines;
      var currFunctionLineNum;
      var unparsedFunctions = [];
      for (var i = 0; i < lines.length; i++) {
        var line = lines[i];
        if (!parseFunctions && /^define .*/.test(line)) {
          inFunction = true;
          currFunctionLines = [];
          currFunctionLineNum = i + 1;
        }
        if (!inFunction || parseFunctions) {
          if (inContinual || new RegExp(/^\ +to.*/g).test(line)) {
            // to after invoke
            ret.slice(-1)[0].lineText += line;
            if (new RegExp(/^\ +\]/g).test(line)) { // end of llvm switch
              inContinual = false;
            }
          } else {
            ret.push({
              lineText: line,
              lineNum: i + 1 + baseLineNum
            });
            if (new RegExp(/^\ +switch\ .*/g).test(line)) {
              // beginning of llvm switch
              inContinual = true;
            }
          }
        } else {
          currFunctionLines.push(line);
        }
        if (!parseFunctions && /^}.*/.test(line)) {
          inFunction = false;
          if (!parseFunctions) {
            var func = funcHeader.processItem(tokenizer.processItem({ lineText: currFunctionLines[0], lineNum: currFunctionLineNum }, true))[0];
            unparsedFunctions.push({
              intertype: 'unparsedFunction',
              // We need this early, to know basic function info - ident, params, varargs
              ident: toNiceIdent(func.ident),
              params: func.params,
              hasVarArgs: func.hasVarArgs,
              lineNum: currFunctionLineNum,
              lines: currFunctionLines
            });
            currFunctionLines = [];
          }
        }
      }
      this.forwardItems(ret.filter(function(item) { return item.lineText; }), 'Tokenizer');
      return unparsedFunctions;
    }
  });

  var ENCLOSER_STARTERS = set('[', '(', '<');
  var ENCLOSER_ENDERS = {
    '[': ']',
    '(': ')',
    '<': '>'
  };

  // Line tokenizer
  tokenizer = substrate.addActor('Tokenizer', {
    processItem: function(item, inner) {
      //assert(item.lineNum != 40000);
      //if (item.lineNum) print(item.lineNum);
      var tokens = [];
      var quotes = 0;
      var lastToken = null;
      var CHUNKSIZE = 64; // How much forward to peek forward. Too much means too many string segments copied
      // Note: '{' is not an encloser, as its use in functions is split over many lines
      var enclosers = {
        '[': 0,
        ']': '[',
        '(': 0,
        ')': '(',
        '<': 0,
        '>': '<'
      };
      var totalEnclosing = 0;
      var that = this;
      function makeToken(text) {
        if (text.length == 0) return;
        // merge certain tokens
        if ( (lastToken && lastToken.text == '%' && text[0] == '"' ) ||
             (lastToken && text.replace(/\*/g, '') == '') ) {
          lastToken.text += text;
          return;
        }

        var token = {
          text: text
        };
        if (text[0] in enclosers) {
          token.item = that.processItem({
            lineText: text.substr(1, text.length-2)
          }, true);
          token.type = text[0];
        }
        // merge certain tokens
        if (lastToken && isType(lastToken.text) && isFunctionDef(token)) {
          lastToken.text += ' ' + text;
        } else if (lastToken && text[text.length-1] == '}') {
          var openBrace = tokens.length-1;
          while (tokens[openBrace].text != '{') openBrace --;
          token = combineTokens(tokens.slice(openBrace+1));
          tokens.splice(openBrace, tokens.length-openBrace+1);
          tokens.push(token);
          token.type = '{';
          lastToken = token;
        } else {
          tokens.push(token);
          lastToken = token;
        }
      }
      // Split using meaningful characters
      var lineText = item.lineText + ' ';
      var re = /[\[\]\(\)<>, "]/g;
      var segments = lineText.split(re);
      segments.pop();
      var len = segments.length;
      var i = -1;
      var curr = '';
      var segment, letter;
      for (var s = 0; s < len; s++) {
        segment = segments[s];
        i += segment.length + 1;
        letter = lineText[i];
        curr += segment;
        switch (letter) {
          case ' ':
            if (totalEnclosing == 0 && quotes == 0) {
              makeToken(curr);
              curr = '';
            } else {
              curr += ' ';
            }
            break;
          case '"':
            if (totalEnclosing == 0) {
              if (quotes == 0) {
                if (curr == '@' || curr == '%') {
                  curr += '"';
                } else {
                  makeToken(curr);
                  curr = '"';
                }
              } else {
                makeToken(curr + '"');
                curr = '';
              }
            } else {
              curr += '"';
            }
            quotes = 1-quotes;
            break;
          case ',':
            if (totalEnclosing == 0 && quotes == 0) {
              makeToken(curr);
              curr = '';
              tokens.push({ text: ',' });
            } else {
              curr += ',';
            }
            break;
          default:
            assert(letter in enclosers);
            if (quotes) {
              curr += letter;
              break;
            }
            if (letter in ENCLOSER_STARTERS) {
              if (totalEnclosing == 0) {
                makeToken(curr);
                curr = '';
              }
              curr += letter;
              enclosers[letter]++;
              totalEnclosing++;
            } else {
              enclosers[enclosers[letter]]--;
              totalEnclosing--;
              if (totalEnclosing == 0) {
                makeToken(curr + letter);
                curr = '';
              } else {
                curr += letter;
              }
            }
        }
      }
      var newItem = {
        tokens: tokens,
        indent: lineText.search(/[^ ]/),
        lineNum: item.lineNum
      };
      if (inner) {
        return newItem;
      } else {
        this.forwardItem(newItem, 'Triager');
      }
      return null;
    }
  });

  MATHOPS = set(['add', 'sub', 'sdiv', 'udiv', 'mul', 'icmp', 'zext', 'urem', 'srem', 'fadd', 'fsub', 'fmul', 'fdiv', 'fcmp', 'uitofp', 'sitofp', 'fpext', 'fptrunc', 'fptoui', 'fptosi', 'trunc', 'sext', 'select', 'shl', 'shr', 'ashl', 'ashr', 'lshr', 'lshl', 'xor', 'or', 'and', 'ptrtoint', 'inttoptr']);

  substrate.addActor('Triager', {
    processItem: function(item) {
      function triage() {
        if (!item.intertype) {
          var token0Text = item.tokens[0].text;
          var token1Text = item.tokens[1] ? item.tokens[1].text : null;
          var tokensLength = item.tokens.length;
          if (item.indent === 2) {
            if (tokensLength >= 5 &&
                (token0Text == 'store' || token1Text == 'store'))
              return 'Store';
            if (tokensLength >= 3 && findTokenText(item, '=') >= 0)
              return 'Assign';
            if (tokensLength >= 3 && token0Text == 'br')
              return 'Branch';
            if (tokensLength >= 2 && token0Text == 'ret')
              return 'Return';
            if (tokensLength >= 2 && token0Text == 'switch')
              return 'Switch';
            if (token0Text == 'unreachable')
              return 'Unreachable';
            if (tokensLength >= 3 && token0Text == 'indirectbr')
              return 'IndirectBr';
          } else if (item.indent === -1) {
            if (tokensLength >= 3 &&
                (token0Text == 'load' || token1Text == 'load'))
              return 'Load';
            if (tokensLength >= 3 &&
                token0Text in MATHOPS)
              return 'Mathops';
            if (tokensLength >= 3 && token0Text == 'bitcast')
              return 'Bitcast';
            if (tokensLength >= 3 && token0Text == 'getelementptr')
              return 'GEP';
            if (tokensLength >= 2 && token0Text == 'alloca')
              return 'Alloca';
            if (tokensLength >= 3 && token0Text == 'extractvalue')
              return 'ExtractValue';
            if (tokensLength >= 3 && token0Text == 'phi')
              return 'Phi';
          } else if (item.indent === 0) {
            if ((tokensLength >= 1 && token0Text.substr(-1) == ':') || // LLVM 2.7 format, or llvm-gcc in 2.8
                (tokensLength >= 3 && token1Text == '<label>'))
              return 'Label';
            if (tokensLength >= 4 && token0Text == 'declare')
              return 'External';
            if (tokensLength >= 3 && token1Text == '=')
              return 'Global';
            if (tokensLength >= 4 && token0Text == 'define' &&
               item.tokens.slice(-1)[0].text == '{')
              return 'FuncHeader';
            if (tokensLength >= 1 && token0Text == '}')
              return 'FuncEnd';
          }
          if (tokensLength >= 3 && (token0Text == 'call' || token1Text == 'call'))
            return 'Call';
          if (token0Text in set(';', 'target'))
            return '/dev/null';
          if (tokensLength >= 3 && token0Text == 'invoke')
            return 'Invoke';
        } else {
          // Already intertyped
          if (item.parentSlot)
            return 'Reintegrator';
        }
        throw 'Invalid token, cannot triage: ' + dump(item);
      }
      this.forwardItem(item, triage(item));
    }
  });

  // Line parsers to intermediate form

  // globals: type or variable
  substrate.addActor('Global', {
    processItem: function(item) {
      function scanConst(value, type) {
        //dprint('inter-const: ' + item.lineNum + ' : ' + JSON.stringify(value) + ',' + type + '\n');
        if (Runtime.isNumberType(type) || pointingLevels(type) >= 1) {
          return { value: toNiceIdent(value.text), type: type };
        } else if (value.text in set('zeroinitializer', 'undef')) { // undef doesn't really need initting, but why not
          return { intertype: 'emptystruct', type: type };
        } else if (value.text && value.text[0] == '"') {
          return { intertype: 'string', text: value.text.substr(1, value.text.length-2) };
        } else {
          // Gets an array of constant items, separated by ',' tokens
          function handleSegments(tokens) {
            // Handle a single segment (after comma separation)
            function handleSegment(segment) {
              if (segment[1].text == 'null') {
                return { intertype: 'value', value: 0, type: 'i32' };
              } else if (segment[1].text == 'zeroinitializer') {
                return { intertype: 'emptystruct', type: segment[0].text };
              } else if (segment[1].text in PARSABLE_LLVM_FUNCTIONS) {
                return parseLLVMFunctionCall(segment);
              } else if (segment[1].type && segment[1].type == '{') {
                return { intertype: 'struct', type: segment[0].text, contents: handleSegments(segment[1].tokens) };
              } else if (segment[1].type && segment[1].type == '<') {
                return { intertype: 'struct', type: segment[0].text, contents: handleSegments(segment[1].item.tokens[0].tokens) };
              } else if (segment[1].type && segment[1].type == '[') {
                return { intertype: 'list', type: segment[0].text, contents: handleSegments(segment[1].item.tokens) };
              } else if (segment.length == 2) {
                return { intertype: 'value', type: segment[0].text, value: toNiceIdent(segment[1].text) };
              } else if (segment[1].text === 'c') {
                // string
                var text = segment[2].text;
                text = text.substr(1, text.length-2);
                return { intertype: 'string', text: text, type: 'i8*' };
              } else if (segment[1].text === 'blockaddress') {
                return parseBlockAddress(segment);
              } else {
                throw 'Invalid segment: ' + dump(segment);
              }
            };
            return splitTokenList(tokens).map(handleSegment);
          }
          if (value.type == '<') { // <{ i8 }> etc.
            value = value.item.tokens;
          }
          var contents;
          if (value.item) {
            // list of items
            contents = value.item.tokens;
          } else if (value.type == '{') {
            // struct
            contents = value.tokens;
          } else if (value[0]) {
            contents = value[0];
          } else {
            throw '// interfailzzzzzzzzzzzzzz ' + dump(value.item) + ' ::: ' + dump(value);
          }
          return { intertype: 'segments', contents: handleSegments(contents) };
        }
      }

      if (item.tokens[2].text == 'alias') {
        cleanOutTokensSet(LLVM.LINKAGES, item.tokens, 3);
        cleanOutTokensSet(LLVM.VISIBILITIES, item.tokens, 3);
        return [{
          intertype: 'alias',
          ident: toNiceIdent(item.tokens[0].text),
          aliasee: toNiceIdent(item.tokens[4].text),
          type: item.tokens[3].text,
          lineNum: item.lineNum
        }];
      }
      if (item.tokens[2].text == 'type') {
        var fields = [];
        var packed = false;
        if (Runtime.isNumberType(item.tokens[3].text)) {
          // Clang sometimes has |= i32| instead of |= { i32 }|
          fields = [item.tokens[3].text];
        } else if (item.tokens[3].text != 'opaque') {
          if (item.tokens[3].type == '<') {
            packed = true;
            item.tokens[3] = tokenizer.processItem({ lineText: '{ ' + item.tokens[3].item.tokens[0].text + ' }' }, true).tokens[0];
          }
          var subTokens = item.tokens[3].tokens;
          subTokens.push({text:','});
          while (subTokens[0]) {
            var stop = 1;
            while ([','].indexOf(subTokens[stop].text) == -1) stop ++;
            fields.push(combineTokens(subTokens.slice(0, stop)).text);
            subTokens.splice(0, stop+1);
          }
        }
        return [{
          intertype: 'type',
          name_: item.tokens[0].text,
          fields: fields,
          packed: packed,
          lineNum: item.lineNum
        }];
      } else {
        // variable
        var ident = item.tokens[0].text;
        cleanOutTokensSet(LLVM.GLOBAL_MODIFIERS, item.tokens, 3);
        cleanOutTokensSet(LLVM.GLOBAL_MODIFIERS, item.tokens, 2);
        var external = false;
        if (item.tokens[2].text === 'external') {
          external = true;
          item.tokens.splice(2, 1);
        }
        var ret = {
          intertype: 'globalVariable',
          ident: toNiceIdent(ident),
          type: item.tokens[2].text,
          external: external,
          lineNum: item.lineNum
        };
        Types.needAnalysis[ret.type] = 0;
        if (ident == '@llvm.global_ctors') {
          ret.ctors = [];
          if (item.tokens[3].item) {
            var subTokens = item.tokens[3].item.tokens;
            splitTokenList(subTokens).forEach(function(segment) {
              ret.ctors.push(segment[1].tokens.slice(-1)[0].text);
            });
          }
        } else {
          if (!item.tokens[3]) throw 'Did you run llvm-dis with -show-annotations? (b)';
          if (item.tokens[3].text == 'c')
            item.tokens.splice(3, 1);
          if (item.tokens[3].text in PARSABLE_LLVM_FUNCTIONS) {
            ret.value = parseLLVMFunctionCall(item.tokens.slice(2));
          } else if (!external) {
            ret.value = scanConst(item.tokens[3], ret.type);
          }
        }
        return [ret];
      }
    }
  });
  // function header
  funcHeader = substrate.addActor('FuncHeader', {
    processItem: function(item) {
      item.tokens = item.tokens.filter(function(token) {
        return !(token.text in LLVM.LINKAGES || token.text in LLVM.PARAM_ATTR || token.text in set('hidden', 'nounwind', 'define', 'inlinehint', '{') || token.text in LLVM.CALLING_CONVENTIONS);
      });
      var ret = {
        intertype: 'function',
        ident: toNiceIdent(item.tokens[1].text),
        returnType: item.tokens[0].text,
        params: parseParamTokens(item.tokens[2].item.tokens),
        lineNum: item.lineNum
      };
      ret.hasVarArgs = false;
      ret.paramIdents = ret.params.map(function(param) {
        if (param.intertype == 'varargs') {
          ret.hasVarArgs = true;
          return null;
        }
        return toNiceIdent(param.ident);
      }).filter(function(param) { return param != null });;
      return [ret];
    }
  });
  // label
  substrate.addActor('Label', {
    processItem: function(item) {
      return [{
        intertype: 'label',
        ident: toNiceIdent(
          item.tokens[0].text.substr(-1) == ':' ?
            '%' + item.tokens[0].text.substr(0, item.tokens[0].text.length-1) :
            '%' + item.tokens[2].text.substr(1)
        ),
        lineNum: item.lineNum
      }];
    }
  });

  // assignment
  substrate.addActor('Assign', {
    processItem: function(item) {
      var opIndex = findTokenText(item, '=');
      var pair = splitItem({
        intertype: 'assign',
        ident: toNiceIdent(combineTokens(item.tokens.slice(0, opIndex)).text),
        lineNum: item.lineNum
      }, 'value');
      this.forwardItem(pair.parent, 'Reintegrator');
      this.forwardItem(mergeInto(pair.child, { // Additional token, to be triaged and later re-integrated
        indent: -1,
        tokens: item.tokens.slice(opIndex+1)
      }), 'Triager');
    }
  });

  substrate.addActor('Reintegrator', makeReintegrator(function(parent, child) {
    this.forwardItem(parent, '/dev/stdout');
  }));

  // 'load'
  substrate.addActor('Load', {
    processItem: function(item) {
      item.intertype = 'load';
      if (item.tokens[0].text == 'volatile') item.tokens.shift(0);
      item.pointerType = item.tokens[1].text;
      item.valueType = item.type = removePointing(item.pointerType);
      Types.needAnalysis[item.type] = 0;
      var last = getTokenIndexByText(item.tokens, ';');
      item.pointer = parseLLVMSegment(item.tokens.slice(1, last)); // TODO: Use this everywhere else too
      item.ident = item.pointer.ident || null;
      this.forwardItem(item, 'Reintegrator');
    }
  });
  // 'extractvalue'
  substrate.addActor('ExtractValue', {
    processItem: function(item) {
      var last = getTokenIndexByText(item.tokens, ';');
      item.intertype = 'extractvalue';
      item.type = item.tokens[1].text; // Of the origin aggregate - not what we extract from it. For that, can only infer it later
      Types.needAnalysis[item.type] = 0;
      item.ident = toNiceIdent(item.tokens[2].text);
      item.indexes = splitTokenList(item.tokens.slice(4, last));
      this.forwardItem(item, 'Reintegrator');
    }
  });
  // 'bitcast'
  substrate.addActor('Bitcast', {
    processItem: function(item) {
      item.intertype = 'bitcast';
      item.type = item.tokens[4].text; // The final type
      Types.needAnalysis[item.type] = 0;
      item.ident = toNiceIdent(item.tokens[2].text);
      item.type2 = item.tokens[1].text; // The original type
      Types.needAnalysis[item.type2] = 0;
      this.forwardItem(item, 'Reintegrator');
    }
  });
  // 'getelementptr'
  substrate.addActor('GEP', {
    processItem: function(item) {
      var first = 0;
      while (!isType(item.tokens[first].text)) first++;
      var last = getTokenIndexByText(item.tokens, ';');
      var segment = [ item.tokens[first], { text: 'getelementptr' }, null, { item: {
        tokens: item.tokens.slice(first, last)
      } } ];
      var data = parseLLVMFunctionCall(segment);
      item.intertype = 'getelementptr';
      item.type = '*'; // We need type info to determine this - all we know is it's a pointer
      item.params = data.params;
      item.ident = data.ident;
      this.forwardItem(item, 'Reintegrator');
    }
  });
  // 'call', 'invoke'
  function makeCall(item, type) {
    item.intertype = type;
    if (['tail'].indexOf(item.tokens[0].text) != -1) {
      item.tokens.splice(0, 1);
    }
    assertEq(item.tokens[0].text, type);
    while (item.tokens[1].text in LLVM.PARAM_ATTR || item.tokens[1].text in LLVM.CALLING_CONVENTIONS) {
      item.tokens.splice(1, 1);
    }
    item.type = item.tokens[1].text;
    Types.needAnalysis[item.type] = 0;
    item.functionType = '';
    while (['@', '%'].indexOf(item.tokens[2].text[0]) == -1 && !(item.tokens[2].text in PARSABLE_LLVM_FUNCTIONS)) {
      // We cannot compile assembly. If you hit this, perhaps tell the compiler not
      // to generate arch-specific code? |-U__i386__ -U__x86_64__| might help, it undefines
      // the standard archs.
      assert(item.tokens[2].text != 'asm', 'Inline assembly cannot be compiled to JavaScript!');
      
      item.functionType += item.tokens[2].text;
      item.tokens.splice(2, 1);
    }
    var tokensLeft = item.tokens.slice(2);
    item.ident = eatLLVMIdent(tokensLeft);
    // We cannot compile assembly, see above.
    assert(item.ident != 'asm', 'Inline assembly cannot be compiled to JavaScript!');
    if (item.ident.substr(-2) == '()') {
      // See comment in isStructType()
      item.ident = item.ident.substr(0, item.ident.length-2);
      // Also, we remove some spaces which might occur.
      while (item.ident[item.ident.length-1] == ' ') {
        item.ident = item.ident.substr(0, item.ident.length-1);
      }
      item.params = [];
    } else {
      item.params = parseParamTokens(tokensLeft[0].item.tokens);
    }
    item.ident = toNiceIdent(item.ident);
    if (type === 'invoke') {
      cleanOutTokens(['alignstack', 'alwaysinline', 'inlinehint', 'naked', 'noimplicitfloat', 'noinline', 'alwaysinline attribute.', 'noredzone', 'noreturn', 'nounwind', 'optsize', 'readnone', 'readonly', 'ssp', 'sspreq'], item.tokens, 4);
      item.toLabel = toNiceIdent(item.tokens[6].text);
      item.unwindLabel = toNiceIdent(item.tokens[9].text);
    }
    if (item.indent == 2) {
      // standalone call - not in assign
      item.standalone = true;
      return [item];
    }
    this.forwardItem(item, 'Reintegrator');
    return null;
  }
  substrate.addActor('Call', {
    processItem: function(item) {
      return makeCall.call(this, item, 'call');
    }
  });
  substrate.addActor('Invoke', {
    processItem: function(item) {
      return makeCall.call(this, item, 'invoke');
    }
  });
  // 'alloca'
  substrate.addActor('Alloca', {
    processItem: function(item) {
      item.intertype = 'alloca';
      item.allocatedType = item.tokens[1].text;
      item.allocatedNum = (item.tokens.length > 3 && Runtime.isNumberType(item.tokens[3].text)) ? toNiceIdent(item.tokens[4].text) : 1;
      item.type = addPointing(item.tokens[1].text); // type of pointer we will get
      Types.needAnalysis[item.type] = 0;
      item.type2 = item.tokens[1].text; // value we will create, and get a pointer to
      Types.needAnalysis[item.type2] = 0;
      this.forwardItem(item, 'Reintegrator');
    }
  });
  // 'phi'
  substrate.addActor('Phi', {
    processItem: function(item) {
      item.intertype = 'phi';
      item.type = item.tokens[1].text;
      var typeToken = [item.tokens[1]];
      Types.needAnalysis[item.type] = 0;
      var last = getTokenIndexByText(item.tokens, ';');
      item.params = splitTokenList(item.tokens.slice(2, last)).map(function(segment) {
        var subSegments = splitTokenList(segment[0].item.tokens);
        var ret = {
          intertype: 'phiparam',
          label: toNiceIdent(subSegments[1][0].text),
          value: parseLLVMSegment(typeToken.concat(subSegments[0]))
        };
        return ret;
      });
      this.forwardItem(item, 'Reintegrator');
    }
  });
  // mathops
  substrate.addActor('Mathops', {
    processItem: function(item) {
      item.intertype = 'mathop';
      item.op = item.tokens[0].text;
      item.variant = null;
      while (item.tokens[1].text in set('nsw', 'nuw')) item.tokens.splice(1, 1);
      if (['icmp', 'fcmp'].indexOf(item.op) != -1) {
        item.variant = item.tokens[1].text;
        item.tokens.splice(1, 1);
      }
      if (item.tokens[1].text == 'exact') item.tokens.splice(1, 1); // TODO: Implement trap values
      var segments = splitTokenList(item.tokens.slice(1));
      for (var i = 1; i <= 4; i++) {
        if (segments[i-1]) {
          item['param'+i] = parseLLVMSegment(segments[i-1]);
        }
      }
      if (item.op === 'select') {
        assert(item.param2.type === item.param3.type);
        item.type = item.param2.type;
      } else if (item.op === 'inttoptr' || item.op === 'ptrtoint') {
        item.type = item.param2.type;
      } else {
        item.type = item.param1.type;
      }
      for (var i = 1; i <= 4; i++) {
        if (item['param'+i]) item['param'+i].type = item.type; // All params have the same type
      }
      Types.needAnalysis[item.type] = 0;
      this.forwardItem(item, 'Reintegrator');
    }
  });
  // 'store'
  substrate.addActor('Store', {
    processItem: function(item) {
      if (item.tokens[0].text == 'volatile') item.tokens.shift(0);
      var segments = splitTokenList(item.tokens.slice(1));
      var ret = {
        intertype: 'store',
        valueType: item.tokens[1].text,
        value: parseLLVMSegment(segments[0]), // TODO: Make everything use this method, with finalizeLLVMParameter too
        pointer: parseLLVMSegment(segments[1]),
        lineNum: item.lineNum
      };
      Types.needAnalysis[ret.valueType] = 0;
      ret.ident = toNiceIdent(ret.pointer.ident);
      ret.pointerType = ret.pointer.type;
      Types.needAnalysis[ret.pointerType] = 0;
      return [ret];
    }
  });
  // 'br'
  substrate.addActor('Branch', {
    processItem: function(item) {
      if (item.tokens[1].text == 'label') {
        return [{
          intertype: 'branch',
          label: toNiceIdent(item.tokens[2].text),
          lineNum: item.lineNum
        }];
      } else {
        var commaIndex = findTokenText(item, ',');
        return [{
          intertype: 'branch',
          condition: parseLLVMSegment(item.tokens.slice(1, commaIndex)),
          labelTrue: toNiceIdent(item.tokens[commaIndex+2].text),
          labelFalse: toNiceIdent(item.tokens[commaIndex+5].text),
          lineNum: item.lineNum
        }];
      }
    }
  });
  // 'ret'
  substrate.addActor('Return', {
    processItem: function(item) {
      var type = item.tokens[1].text;
      Types.needAnalysis[type] = 0;
      return [{
        intertype: 'return',
        type: type,
        value: (item.tokens[2] && type !== 'void') ? parseLLVMSegment(item.tokens.slice(1)) : null,
        lineNum: item.lineNum
      }];
    }
  });
  // 'switch'
  substrate.addActor('Switch', {
    processItem: function(item) {
      function parseSwitchLabels(item) {
        var ret = [];
        var tokens = item.item.tokens;
        while (tokens.length > 0) {
          ret.push({
            value: tokens[1].text,
            label: toNiceIdent(tokens[4].text)
          });
          tokens = tokens.slice(5);
        }
        return ret;
      }
      var type = item.tokens[1].text;
      Types.needAnalysis[type] = 0;
      return [{
        intertype: 'switch',
        type: type,
        ident: toNiceIdent(item.tokens[2].text),
        defaultLabel: toNiceIdent(item.tokens[5].text),
        switchLabels: parseSwitchLabels(item.tokens[6]),
        lineNum: item.lineNum
      }];
    }
  });
  // function end
  substrate.addActor('FuncEnd', {
    processItem: function(item) {
      return [{
        intertype: 'functionEnd',
        lineNum: item.lineNum
      }];
    }
  });
  // external function stub
  substrate.addActor('External', {
    processItem: function(item) {
      if (item.tokens[1].text in LLVM.LINKAGES || item.tokens[1].text in LLVM.PARAM_ATTR) {
        item.tokens.splice(1, 1);
      }
      return [{
        intertype: 'functionStub',
        ident: toNiceIdent(item.tokens[2].text),
        returnType: item.tokens[1],
        params: item.tokens[3],
        lineNum: item.lineNum
      }];
    }
  });
  // 'unreachable'
  substrate.addActor('Unreachable', {
    processItem: function(item) {
      return [{
        intertype: 'unreachable',
        lineNum: item.lineNum
      }];
    }
  });
  // 'indirectbr'
  substrate.addActor('IndirectBr', {
    processItem: function(item) {
      var ret = {
        intertype: 'indirectbr',
        pointer: parseLLVMSegment(splitTokenList(item.tokens.slice(1))[0]),
        type: item.tokens[1].text,
        lineNum: item.lineNum
      };
      Types.needAnalysis[ret.type] = 0;
      return [ret];
    }
  });

  // Input

  substrate.addItem({
    llvmLines: data
  }, 'LineSplitter');

  return substrate.solve();
}

