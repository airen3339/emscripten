import os, multiprocessing, subprocess
from runner import BrowserCore, path_from_root
from tools.shared import *

def clean_pids(pids):
  import signal, errno
  def pid_exists(pid):
    try:
      # NOTE: may just kill the process in Windows
      os.kill(pid, 0)
    except OSError, e:
      return e.errno == errno.EPERM
    else:
        return True
  def kill_pids(pids, sig):
    for pid in pids:
      if not pid_exists(pid):
        break
      print '[killing %d]' % pid
      try:
        os.kill(pid, sig)
        print '[kill succeeded]'
      except:
        print '[kill fail]'
  # ask nicely (to try and catch the children)
  kill_pids(pids, signal.SIGTERM)
  time.sleep(1)
  # extreme prejudice, may leave children
  kill_pids(pids, signal.SIGKILL)

def make_relay_server(port1, port2):
  print >> sys.stderr, 'creating relay server on ports %d,%d' % (port1, port2)
  proc = Popen([PYTHON, path_from_root('tests', 'sockets', 'socket_relay.py'), str(port1), str(port2)])
  return proc

class WebsockifyServerHarness:
  def __init__(self, filename, args, listen_port, target_port):
    self.pids = []
    self.filename = filename
    self.target_port = target_port
    self.listen_port = listen_port
    self.args = args or []

  def __enter__(self):
    import socket, websockify

    # compile the server
    # NOTE empty filename support is a hack to support
    # the current test_enet
    if self.filename:
      Popen([CLANG_CC, path_from_root('tests', self.filename), '-o', 'server'] + self.args).communicate()
      process = Popen([os.path.abspath('server')])
      self.pids.append(process.pid)

    # start the websocket proxy
    print >> sys.stderr, 'running websockify on %d, forward to tcp %d' % (self.listen_port, self.target_port)
    wsp = websockify.WebSocketProxy(verbose=True, listen_port=self.listen_port, target_host="127.0.0.1", target_port=self.target_port, run_once=True)
    self.websockify = multiprocessing.Process(target=wsp.start_server)
    self.websockify.start()
    self.pids.append(self.websockify.pid)
    print '[Websockify on process %s]' % str(self.pids[-2:])

  def __exit__(self, *args, **kwargs):
    # try to kill the websockify proxy gracefully
    if self.websockify.is_alive():
      self.websockify.terminate()
    self.websockify.join()

    # clean up any processes we started
    clean_pids(self.pids)


class CompiledServerHarness:
  def __init__(self, filename, args):
    self.pids = []
    self.filename = filename
    self.args = args or []

  def __enter__(self):
    import socket, websockify

    # compile the server
    Popen([PYTHON, EMCC, path_from_root('tests', self.filename), '-o', 'server.js'] + self.args).communicate()
    process = Popen([NODE_JS, 'server.js'])
    self.pids.append(process.pid)

  def __exit__(self, *args, **kwargs):
    # clean up any processes we started
    clean_pids(self.pids)

    # always run these tests last
    # make sure to use different ports in each one because it takes a while for the processes to be cleaned up

    # NOTE all datagram tests are temporarily disabled, as
    # we can't truly test datagram sockets until we have
    # proper listen server support.

class sockets(BrowserCore):
  def test_inet(self):
    src = r'''
      #include <stdio.h>
      #include <arpa/inet.h>

      int main() {
        printf("*%x,%x,%x,%x,%x,%x*\n", htonl(0xa1b2c3d4), htonl(0xfe3572e0), htonl(0x07abcdf0), htons(0xabcd), ntohl(0x43211234), ntohs(0xbeaf));
        in_addr_t i = inet_addr("190.180.10.78");
        printf("%x\n", i);
        return 0;
      }
    '''
    self.do_run(src, '*d4c3b2a1,e07235fe,f0cdab07,cdab,34122143,afbe*\n4e0ab4be\n')

  def test_inet2(self):
    src = r'''
      #include <stdio.h>
      #include <arpa/inet.h>

      int main() {
        struct in_addr x, x2;
        int *y = (int*)&x;
        *y = 0x12345678;
        printf("%s\n", inet_ntoa(x));
        int r = inet_aton(inet_ntoa(x), &x2);
        printf("%s\n", inet_ntoa(x2));
        return 0;
      }
    '''
    self.do_run(src, '120.86.52.18\n120.86.52.18\n')

  def test_inet3(self):
    src = r'''
      #include <stdio.h>
      #include <arpa/inet.h>
      #include <sys/socket.h>
      int main() {
        char dst[64];
        struct in_addr x, x2;
        int *y = (int*)&x;
        *y = 0x12345678;
        printf("%s\n", inet_ntop(AF_INET,&x,dst,sizeof dst));
        int r = inet_aton(inet_ntoa(x), &x2);
        printf("%s\n", inet_ntop(AF_INET,&x2,dst,sizeof dst));
        return 0;
      }
    '''
    self.do_run(src, '120.86.52.18\n120.86.52.18\n')

  def test_inet4(self):
    if Settings.USE_TYPED_ARRAYS != 2: return self.skip('requires ta2')

    src = r'''
      #include <stdio.h>
      #include <arpa/inet.h>
      #include <sys/socket.h>

      void test(char *test_addr){
          char str[40];
          struct in6_addr addr;
          unsigned char *p = (unsigned char*)&addr;
          int ret;
          ret = inet_pton(AF_INET6,test_addr,&addr);
          if(ret == -1) return;
          if(ret == 0) return;
          if(inet_ntop(AF_INET6,&addr,str,sizeof(str)) == NULL ) return;
          printf("%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x - %s\n",
               p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15],str);
      }
      int main(){
          test("::");
          test("::1");
          test("::1.2.3.4");
          test("::17.18.19.20");
          test("::ffff:1.2.3.4");
          test("1::ffff");
          test("::255.255.255.255");
          test("0:ff00:1::");
          test("0:ff::");
          test("abcd::");
          test("ffff::a");
          test("ffff::a:b");
          test("ffff::a:b:c");
          test("ffff::a:b:c:d");
          test("ffff::a:b:c:d:e");
          test("::1:2:0:0:0");
          test("0:0:1:2:3::");
          test("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
          test("1::255.255.255.255");

          //below should fail and not produce results..
          test("1.2.3.4");
          test("");
          test("-");
      }
    '''
    self.do_run(src,
        "0000:0000:0000:0000:0000:0000:0000:0000 - ::\n"
        "0000:0000:0000:0000:0000:0000:0000:0001 - ::1\n"
        "0000:0000:0000:0000:0000:0000:0102:0304 - ::1.2.3.4\n"
        "0000:0000:0000:0000:0000:0000:1112:1314 - ::17.18.19.20\n"
        "0000:0000:0000:0000:0000:ffff:0102:0304 - ::ffff:1.2.3.4\n"
        "0001:0000:0000:0000:0000:0000:0000:ffff - 1::ffff\n"
        "0000:0000:0000:0000:0000:0000:ffff:ffff - ::255.255.255.255\n"
        "0000:ff00:0001:0000:0000:0000:0000:0000 - 0:ff00:1::\n"
        "0000:00ff:0000:0000:0000:0000:0000:0000 - 0:ff::\n"
        "abcd:0000:0000:0000:0000:0000:0000:0000 - abcd::\n"
        "ffff:0000:0000:0000:0000:0000:0000:000a - ffff::a\n"
        "ffff:0000:0000:0000:0000:0000:000a:000b - ffff::a:b\n"
        "ffff:0000:0000:0000:0000:000a:000b:000c - ffff::a:b:c\n"
        "ffff:0000:0000:0000:000a:000b:000c:000d - ffff::a:b:c:d\n"
        "ffff:0000:0000:000a:000b:000c:000d:000e - ffff::a:b:c:d:e\n"
        "0000:0000:0000:0001:0002:0000:0000:0000 - ::1:2:0:0:0\n"
        "0000:0000:0001:0002:0003:0000:0000:0000 - 0:0:1:2:3::\n"
        "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff - ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff\n"
        "0001:0000:0000:0000:0000:0000:ffff:ffff - 1::ffff:ffff\n"
    )

  def test_getaddrinfo(self):
    self.do_run(open(path_from_root('tests', 'sockets', 'test_getaddrinfo.c')).read(), 'success')

  def test_getnameinfo(self):
    self.do_run(open(path_from_root('tests', 'sockets', 'test_getnameinfo.c')).read(), 'success')

  def test_gethostbyname(self):
    self.do_run(open(path_from_root('tests', 'sockets', 'test_gethostbyname.c')).read(), 'success')

  def test_sockets_echo(self):
    sockets_include = '-I'+path_from_root('tests', 'sockets')

    for datagram in [0]:
      dgram_define = '-DTEST_DGRAM=%d' % datagram

      for harness in [
        WebsockifyServerHarness(os.path.join('sockets', 'test_sockets_echo_server.c'), ['-DSOCKK=8990', dgram_define, sockets_include], 8991, 8990)
        # CompiledServerHarness(os.path.join('sockets', 'test_sockets_echo_server.c'), ['-DSOCKK=8990', dgram_define, sockets_include])
      ]:
        with harness:
          self.btest(os.path.join('sockets', 'test_sockets_echo_client.c'), expected='0', args=['-DSOCKK=8991', dgram_define, sockets_include])

  def test_sockets_echo_bigdata(self):
    sockets_include = '-I'+path_from_root('tests', 'sockets')

    for datagram in [0]:
      dgram_define = '-DTEST_DGRAM=%d' % datagram

      # generate a large string literal to use as our message
      message = ''
      for i in range(256*256*2):
          message += str(unichr(ord('a') + (i % 26)))

      # re-write the client test with this literal (it's too big to pass via command line)
      input_filename = path_from_root('tests', 'sockets', 'test_sockets_echo_client.c')
      input = open(input_filename).read()
      output = input.replace('#define MESSAGE "pingtothepong"', '#define MESSAGE "%s"' % message)

      for harness in [
        WebsockifyServerHarness(os.path.join('sockets', 'test_sockets_echo_server.c'), ['-DSOCKK=8992', dgram_define, sockets_include], 8993, 8992)
      ]:
        with harness:
          self.btest(output, expected='0', args=['-DSOCKK=8993', dgram_define, sockets_include], force_c=True)

  def test_sockets_partial(self):
    for harness in [
      WebsockifyServerHarness(os.path.join('sockets', 'test_sockets_partial_server.c'), ['-DSOCKK=8994'], 8995, 8994)
    ]:
      with harness:
        self.btest(os.path.join('sockets', 'test_sockets_partial_client.c'), expected='165', args=['-DSOCKK=8995'])

  def test_sockets_select_server_no_accept(self):
    for harness in [
      WebsockifyServerHarness(os.path.join('sockets', 'test_sockets_select_server_no_accept_server.c'), ['-DSOCKK=8995'], 8996, 8995)
    ]:
      self.btest(os.path.join('sockets', 'test_sockets_select_server_no_accept_client.c'), expected='266', args=['-DSOCKK=8996'])

  def test_sockets_select_server_closes_connection_rw(self):
    sockets_include = '-I'+path_from_root('tests', 'sockets')

    for harness in [
      WebsockifyServerHarness(os.path.join('sockets', 'test_sockets_echo_server.c'), ['-DSOCKK=9004', sockets_include], 9005, 9004)
    ]:
      with harness:
        self.btest(os.path.join('sockets', 'test_sockets_select_server_closes_connection_client_rw.c'), expected='266', args=['-DSOCKK=9005', sockets_include])

  # TODO remove this once we have proper listen server support built into emscripten.
  # being that enet uses datagram sockets, we can't proxy to a native server with
  # websockify, so we're emulating the listen server in the browser and relaying
  # between two TCP servers.
  def test_enet(self):
    try_delete(self.in_dir('enet'))
    shutil.copytree(path_from_root('tests', 'enet'), self.in_dir('enet'))
    pwd = os.getcwd()
    os.chdir(self.in_dir('enet'))
    Popen([PYTHON, path_from_root('emconfigure'), './configure']).communicate()
    Popen([PYTHON, path_from_root('emmake'), 'make']).communicate()
    enet = [self.in_dir('enet', '.libs', 'libenet.a'), '-I'+path_from_root('tests', 'enet', 'include')]
    os.chdir(pwd)
    Popen([PYTHON, EMCC, path_from_root('tests', 'sockets', 'test_enet_server.c'), '-o', 'server.html', '-DSOCKK=2235'] + enet).communicate()

    with WebsockifyServerHarness('', [], 2235, 2234):
      with WebsockifyServerHarness('', [], 2237, 2236):
        pids = []
        try:
          proc = make_relay_server(2234, 2236)
          pids.append(proc.pid)
          self.btest(os.path.join('sockets', 'test_enet_client.c'), expected='0', args=['-DSOCKK=2237'] + enet)
        finally:
          clean_pids(pids);

  # TODO use this once we have listen server support
  # def test_enet(self):
  #   try_delete(self.in_dir('enet'))
  #   shutil.copytree(path_from_root('tests', 'enet'), self.in_dir('enet'))
  #   pwd = os.getcwd()
  #   os.chdir(self.in_dir('enet'))
  #   Popen([PYTHON, path_from_root('emconfigure'), './configure']).communicate()
  #   Popen([PYTHON, path_from_root('emmake'), 'make']).communicate()
  #   enet = [self.in_dir('enet', '.libs', 'libenet.a'), '-I'+path_from_root('tests', 'enet', 'include')]
  #   os.chdir(pwd)

  #   for harness in [
  #     self.CompiledServerHarness(os.path.join('sockets', 'test_enet_server.c'), ['-DSOCKK=9010'] + enet, 9011, 9010)
  #   ]:
  #     with harness:
  #       self.btest(os.path.join('sockets', 'test_enet_client.c'), expected='0', args=['-DSOCKK=9011'] + enet)

  def test_webrtc(self):
    host_src = 'webrtc_host.c'
    peer_src = 'webrtc_peer.c'

    host_outfile = 'host.html'
    peer_outfile = 'peer.html'

    host_filepath = path_from_root('tests', 'sockets', host_src)      
    temp_host_filepath = os.path.join(self.get_dir(), os.path.basename(host_src))
    with open(host_filepath) as f: host_src = f.read()
    with open(temp_host_filepath, 'w') as f: f.write(self.with_report_result(host_src))

    peer_filepath = path_from_root('tests', 'sockets', peer_src)      
    temp_peer_filepath = os.path.join(self.get_dir(), os.path.basename(peer_src))
    with open(peer_filepath) as f: peer_src = f.read()
    with open(temp_peer_filepath, 'w') as f: f.write(self.with_report_result(peer_src))

    open(os.path.join(self.get_dir(), 'host_pre.js'), 'w').write('''
      var Module = {
        webrtc: {
          broker: 'https://mdsw.ch:8080',
          session: undefined,
          onpeer: function(peer, route) {
            window.open('http://localhost:8888/peer.html?' + route);
            // iframe = document.createElement("IFRAME");
            // iframe.setAttribute("src", "http://localhost:8888/peer.html?" + route);
            // iframe.style.display = "none";
            // document.body.appendChild(iframe);
            peer.listen();
          },
          onconnect: function(peer) {
          },
          ondisconnect: function(peer) {
          },
          onerror: function(error) {
            console.error(error);
          }
        },
      };
    ''')

    open(os.path.join(self.get_dir(), 'peer_pre.js'), 'w').write('''
      var Module = {
        webrtc: {
          broker: 'https://mdsw.ch:8080',
          session: window.location.toString().split('?')[1],
          onpeer: function(peer, route) {
            peer.connect(Module['webrtc']['session']);
          },
          onconnect: function(peer) {
          },
          ondisconnect: function(peer) {
            // Calling window.close() from this handler hangs my browser, so run it in the next turn
            setTimeout(window.close, 0);
          },
          onerror: function(error) {
            console.error(error);
          }
        }
      };
    ''')

    Popen([PYTHON, EMCC, temp_host_filepath, '-o', host_outfile] + ['-s', 'GL_TESTING=1', '--pre-js', 'host_pre.js', '-s', 'SOCKET_WEBRTC=1', '-s', 'SOCKET_DEBUG=1']).communicate()
    Popen([PYTHON, EMCC, temp_peer_filepath, '-o', peer_outfile] + ['-s', 'GL_TESTING=1', '--pre-js', 'peer_pre.js', '-s', 'SOCKET_WEBRTC=1', '-s', 'SOCKET_DEBUG=1']).communicate()

    expected = '1'
    self.run_browser(host_outfile, '.', ['/report_result?' + e for e in expected])