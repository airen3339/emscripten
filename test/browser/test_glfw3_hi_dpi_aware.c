/*
 * Copyright 2023 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <emscripten/html5.h>

// installing mock devicePixelRatio (independent of the browser/screen resolution)
static void installMockDevicePixelRatio() {
  printf("installing mock devicePixelRatio...\n");
  EM_ASM(
      Browser.mockDevicePixelRatio = 1.0;
      Browser.getDevicePixelRatio = () => { console.log("mock getDevicePixelRatio"); return Browser.mockDevicePixelRatio; };
      );
}

static void setDevicePixelRatio(float ratio) {
  printf("setDevicePixelRatio %.0f\n", ratio);
  EM_ASM({
    Browser.mockDevicePixelRatio = $0;
    // mocking/simulating the fact that an event should be raised when devicePixelRatio changes
    if (Browser.devicePixelRatioMQS) {
      Browser.onDevicePixelRatioChange();
    }
  }, ratio);
}

static void setBrowserIsHiDPIAware(bool isHiDPIAware) {
  printf("setBrowserIsHiDPIAware %s\n", isHiDPIAware ? "true" : "false");
  EM_ASM({Browser.setHiDPIAware($0)}, isHiDPIAware ? 1 : 0);
}

static void checkWindowSize(GLFWwindow *window, int expectedWidth, int expectedHeight, float ratio) {
  // first check the window size
  int w, h;
  glfwGetWindowSize(window, &w, &h);
  printf("windowSize => %d == %d && %d == %d\n", w, expectedWidth, h, expectedHeight);
  assert(w == expectedWidth && h == expectedHeight);

  // second check the frame buffer size
  int fbw, fbh;
  glfwGetFramebufferSize(window, &fbw, &fbh);
  printf("framebufferSize => %d == %d && %d == %d\n", fbw, (int) (expectedWidth * ratio), fbh, (int) (expectedHeight * ratio));
  assert(fbw == (int) (expectedWidth * ratio) && fbh == (int) (expectedHeight * ratio));
}

static bool getBrowserIsHiDPIAware() {
  return EM_ASM_INT(return Browser.isHiDPIAware ? 1 : 0) != 0;
}

int main() {

  GLFWwindow* window;

  assert(glfwInit() == GL_TRUE);

  installMockDevicePixelRatio();

  // by default, Browser is NOT Hi DPI aware
  assert(!getBrowserIsHiDPIAware());

  // Use case 1: Browser is NOT Hi DPI Aware | devicePixelRatio is 1.0
  // Expected outcome is window size and frame buffer size are the same
  {
    printf("Use case #1\n");
    window = glfwCreateWindow(640, 480, "test_glfw3_hi_dpi_aware.c | #1", NULL, NULL);
    assert(window != NULL);
    assert(!getBrowserIsHiDPIAware());
    checkWindowSize(window, 640, 480, 1.0);
    glfwSetWindowSize(window, 600, 400);
    checkWindowSize(window, 600, 400, 1.0);
    glfwDestroyWindow(window);
  }

  // Use case 2: Browser is NOT Hi DPI Aware | devicePixelRatio is 2.0
  // Expected outcome is window size and frame buffer size are the same (because
  // browser is not Hi DPI Aware)
  {
    printf("Use case #2\n");
    setDevicePixelRatio(2.0);
    window = glfwCreateWindow(640, 480, "test_glfw3_hi_dpi_aware.c | #2", NULL, NULL);
    assert(window != NULL);
    assert(!getBrowserIsHiDPIAware());
    checkWindowSize(window, 640, 480, 1.0);
    glfwSetWindowSize(window, 600, 400);
    checkWindowSize(window, 600, 400, 1.0);
    glfwDestroyWindow(window);
  }

  // Use case 3: Browser is Hi DPI Aware | devicePixelRatio is 1.0
  // Expected outcome is window size and frame buffer size are the same
  {
    printf("Use case #3\n");
    setDevicePixelRatio(1.0);
    setBrowserIsHiDPIAware(true);
    window = glfwCreateWindow(640, 480, "test_glfw3_hi_dpi_aware.c | #3", NULL, NULL);
    assert(window != NULL);
    assert(getBrowserIsHiDPIAware());
    checkWindowSize(window, 640, 480, 1.0);
    glfwSetWindowSize(window, 600, 400);
    checkWindowSize(window, 600, 400, 1.0);
    glfwDestroyWindow(window);
  }

  // Use case 4: Browser is Hi DPI Aware | devicePixelRatio is 2.0
  // Expected outcome is frame buffer size is 2x window size
  {
    printf("Use case #4\n");
    setDevicePixelRatio(2.0);
    setBrowserIsHiDPIAware(true);
    window = glfwCreateWindow(640, 480, "test_glfw3_hi_dpi_aware.c | #4", NULL, NULL);
    assert(window != NULL);
    assert(getBrowserIsHiDPIAware());
    checkWindowSize(window, 640, 480, 2.0);
    glfwSetWindowSize(window, 600, 400);
    checkWindowSize(window, 600, 400, 2.0);
    glfwDestroyWindow(window);
  }

  // Use case 5: Browser Hi DPI Awareness changes | devicePixelRatio 2.0
  // Expected outcome is that the window sizes is adjusted automatically
  {
    printf("Use case #5\n");
    window = glfwCreateWindow(640, 480, "test_glfw3_hi_dpi_aware.c | #5", NULL, NULL);
    assert(window != NULL);
    assert(getBrowserIsHiDPIAware());
    checkWindowSize(window, 640, 480, 2.0);
    setBrowserIsHiDPIAware(false);
    checkWindowSize(window, 640, 480, 1.0);
    glfwDestroyWindow(window);
  }

  // Use case 6: Browser is NOT Hi DPI Aware | devicePixelRatio changes
  // Expected outcome is that the window sizes does not change
  {
    printf("Use case #6\n");
    setDevicePixelRatio(1.0);
    window = glfwCreateWindow(640, 480, "test_glfw3_hi_dpi_aware.c | #6", NULL, NULL);
    assert(window != NULL);
    assert(!getBrowserIsHiDPIAware());
    checkWindowSize(window, 640, 480, 1.0);
    setDevicePixelRatio(2.0);
    checkWindowSize(window, 640, 480, 1.0);
    glfwDestroyWindow(window);
  }

  // Use case 7: Browser is Hi DPI Aware | devicePixelRatio changes
  // Expected outcome is that the window sizes is adjusted automatically
  {
    printf("Use case #7\n");
    setDevicePixelRatio(2.0);
    setBrowserIsHiDPIAware(true);
    window = glfwCreateWindow(640, 480, "test_glfw3_hi_dpi_aware.c | #7", NULL, NULL);
    assert(window != NULL);
    assert(getBrowserIsHiDPIAware());
    checkWindowSize(window, 640, 480, 2.0);
    setDevicePixelRatio(1.0);
    checkWindowSize(window, 640, 480, 1.0);
    glfwDestroyWindow(window);
  }

  glfwTerminate();

  return 0;
}