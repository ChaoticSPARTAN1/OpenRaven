// ravenhdr.c - native HDR10 output on Linux (graphics.ini HDR = 1). Counterpart of win/ravenhdr_win.c.
// The game draws into our RGBA16F screen framebuffer (ravengltrace.c). Each frame a GL pass converts it
// to HDR10 (decode 2.2, cap at peak/paper, 1.0 = paper white, BT.709 -> BT.2020, PQ) into a 10-bit
// texture, reads that into a buffer shared with Vulkan (GL_EXT_memory_object_fd + GL_EXT_semaphore_fd,
// the same pattern as the neural pass), and Vulkan copies it into an HDR10 swapchain
// (A2B10G10R10 + VK_COLOR_SPACE_HDR10_ST2084_EXT) on the game's X11 window and presents.
// An X11 window only gets HDR colour spaces inside gamescope (--hdr-enabled; play_halo.sh starts it when
// HDR = 1). Without them, or if anything fails, the screen framebuffer is blitted to the window and the
// game's own swap runs as before (SDR).
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

typedef unsigned GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef unsigned char GLboolean;
const char *ravengfx_get(const char *key);
int ravenhdr_on(void);
unsigned ravengl_screen_fbo(void);
void ravengl_screen_size(int *w, int *h);

static void *gpa(const char *n) {
    static void *(*g)(const char *);
    if (!g) g = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    void *f = g ? g(n) : NULL;
    return f ? f : dlsym(RTLD_DEFAULT, n);
}
#define G(ret, n, args) static ret (*n) args;
G(void, gGenFramebuffers, (GLsizei, GLuint *)) G(void, gBindFramebuffer, (GLenum, GLuint))
G(void, gFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
G(void, gBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
G(GLuint, gCreateShader, (GLenum)) G(void, gShaderSource, (GLuint, GLsizei, const char *const *, const GLint *))
G(void, gCompileShader, (GLuint)) G(GLuint, gCreateProgram, (void)) G(void, gAttachShader, (GLuint, GLuint))
G(void, gLinkProgram, (GLuint)) G(void, gGetProgramiv, (GLuint, GLenum, GLint *))
G(void, gGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *)) G(void, gUseProgram, (GLuint))
G(GLint, gGetUniformLocation, (GLuint, const char *)) G(void, gUniform1i, (GLint, GLint)) G(void, gUniform1f, (GLint, float))
G(void, gGenVertexArrays, (GLsizei, GLuint *)) G(void, gBindVertexArray, (GLuint)) G(void, gActiveTexture, (GLenum))
G(void, gBindSampler, (GLuint, GLuint)) G(void, gGenTextures, (GLsizei, GLuint *)) G(void, gDeleteTextures, (GLsizei, const GLuint *))
G(void, gBindTexture, (GLenum, GLuint)) G(void, gTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
G(void, gTexParameteri, (GLenum, GLenum, GLint)) G(void, gGetIntegerv, (GLenum, GLint *)) G(GLboolean, gIsEnabled, (GLenum))
G(void, gEnable, (GLenum)) G(void, gDisable, (GLenum)) G(void, gViewport, (GLint, GLint, GLsizei, GLsizei))
G(void, gDrawArrays, (GLenum, GLint, GLsizei)) G(void, gReadBuffer, (GLenum)) G(void, gDrawBuffer, (GLenum))
G(void, gReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) G(void, gColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))
G(void, gGetTexImage, (GLenum, GLint, GLenum, GLenum, void *)) G(void, gBindBuffer, (GLenum, GLuint)) G(void, gGenBuffers, (GLsizei, GLuint *))
G(void, gPixelStorei, (GLenum, GLint)) G(void, gFlush, (void)) G(void, gGetUnsignedBytevEXT, (GLenum, unsigned char *))
G(void, gCreateMemoryObjectsEXT, (GLsizei, GLuint *)) G(void, gImportMemoryFdEXT, (GLuint, uint64_t, GLenum, GLint))
G(void, gBufferStorageMemEXT, (GLenum, intptr_t, GLuint, uint64_t)) G(void, gGenSemaphoresEXT, (GLsizei, GLuint *))
G(void, gImportSemaphoreFdEXT, (GLuint, GLenum, GLint))
G(void, gSignalSemaphoreEXT, (GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *))
G(void *, gXGetCurrentDisplay, (void)) G(unsigned long, gXGetCurrentDrawable, (void))

static const char *vs = "#version 330 core\nvoid main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
// Same math as win/ravenhdr_win.c. Rows are flipped: Vulkan images are top-down.
static const char *fs =
"#version 330 core\n"
"uniform sampler2D S; uniform int h; uniform float paper, peak;\n"
"out vec4 o;\n"
"vec3 pq(vec3 c){ const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
"  vec3 y = pow(max(c, 0.0), vec3(m1)); return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2)); }\n"
"void main(){\n"
"  ivec2 q = ivec2(gl_FragCoord.x, h - 1 - int(gl_FragCoord.y));\n"
"  vec3 c = texelFetch(S, q, 0).rgb;\n"
"  vec3 L = pow(max(c, 0.0), vec3(2.2));\n"
"  L = min(L, vec3(max(peak / paper, 1.0)));\n"
"  L = mat3(0.627402, 0.069095, 0.016394, 0.329292, 0.919544, 0.088028, 0.043306, 0.011360, 0.895578) * L;\n"
"  o = vec4(clamp(pq(L * paper / 10000.0), 0.0, 1.0), 1.0);\n"
"}\n";

// ---- Vulkan ----
#define VF(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
  X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
  X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkCreateXlibSurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define DF(X) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) \
  X(vkDestroyBuffer) X(vkFreeMemory) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
  X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
  X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkQueueSubmit) X(vkCreateSemaphore) X(vkCreateFence) \
  X(vkWaitForFences) X(vkResetFences) X(vkDeviceWaitIdle) X(vkGetMemoryFdKHR) X(vkGetSemaphoreFdKHR) X(vkDestroySemaphore)
#define F(n) static PFN_##n n;
VF(F) DF(F)
static PFN_vkGetInstanceProcAddr gipa; static PFN_vkCreateInstance vkCreateInstance; static PFN_vkSetHdrMetadataEXT vkSetHdrMetadataEXT;

static struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf; VkSurfaceKHR surf;
    VkSwapchainKHR sc; VkImage img[8]; uint32_t nimg; VkExtent2D ext; VkPresentModeKHR pm;
    VkBuffer buf; VkDeviceMemory mem; VkDeviceSize memsz;
    VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkSemaphore sem_gl, sem_acq, sem_done;
    int fence_pending;
} V;
static int st;              // 0 = not started, 1 = running, -1 = off (blit + the game's swap)
static int W, H;            // swapchain / buffer size
static GLuint prog, vao, pqtex, fb_src, fb_dst, srct, glbuf, glmem, glsem;
static float paper = 203, peak = 1000;
static int vsync = 1;

static void off(const char *why) { fprintf(stderr, "[hdr] HDR10 output off: %s. The game shows in SDR.\n", why); st = -1; }

static void read_settings(void) {
    const char *v = ravengfx_get("HDR_PAPERWHITE"); paper = v && atof(v) >= 40 ? (float)atof(v) : 203;
    v = ravengfx_get("HDR_PEAK"); peak = v && atof(v) > 100 ? (float)atof(v) : 1000;
    v = ravengfx_get("VSYNC"); vsync = !v || strcmp(v, "0") != 0;
}

static int load_gl(void) {
#define L(v, n) if (!(v = (__typeof__(v))gpa(n))) { fprintf(stderr, "[hdr] missing %s\n", n); return 0; }
    L(gGenFramebuffers, "glGenFramebuffers") L(gBindFramebuffer, "glBindFramebuffer") L(gFramebufferTexture2D, "glFramebufferTexture2D")
    L(gBlitFramebuffer, "glBlitFramebuffer") L(gCreateShader, "glCreateShader") L(gShaderSource, "glShaderSource")
    L(gCompileShader, "glCompileShader") L(gCreateProgram, "glCreateProgram") L(gAttachShader, "glAttachShader")
    L(gLinkProgram, "glLinkProgram") L(gGetProgramiv, "glGetProgramiv") L(gGetProgramInfoLog, "glGetProgramInfoLog")
    L(gUseProgram, "glUseProgram") L(gGetUniformLocation, "glGetUniformLocation") L(gUniform1i, "glUniform1i") L(gUniform1f, "glUniform1f")
    L(gGenVertexArrays, "glGenVertexArrays") L(gBindVertexArray, "glBindVertexArray") L(gActiveTexture, "glActiveTexture")
    L(gBindSampler, "glBindSampler") L(gGenTextures, "glGenTextures") L(gDeleteTextures, "glDeleteTextures") L(gBindTexture, "glBindTexture")
    L(gTexImage2D, "glTexImage2D") L(gTexParameteri, "glTexParameteri") L(gGetIntegerv, "glGetIntegerv") L(gIsEnabled, "glIsEnabled")
    L(gEnable, "glEnable") L(gDisable, "glDisable") L(gViewport, "glViewport") L(gDrawArrays, "glDrawArrays") L(gReadBuffer, "glReadBuffer")
    L(gDrawBuffer, "glDrawBuffer") L(gReadPixels, "glReadPixels") L(gColorMask, "glColorMask") L(gGetTexImage, "glGetTexImage")
    L(gBindBuffer, "glBindBuffer") L(gGenBuffers, "glGenBuffers") L(gPixelStorei, "glPixelStorei") L(gFlush, "glFlush")
    L(gXGetCurrentDisplay, "glXGetCurrentDisplay") L(gXGetCurrentDrawable, "glXGetCurrentDrawable")
#undef L
    gGetUnsignedBytevEXT = gpa("glGetUnsignedBytevEXT"); gCreateMemoryObjectsEXT = gpa("glCreateMemoryObjectsEXT");
    gImportMemoryFdEXT = gpa("glImportMemoryFdEXT"); gBufferStorageMemEXT = gpa("glBufferStorageMemEXT");
    gGenSemaphoresEXT = gpa("glGenSemaphoresEXT"); gImportSemaphoreFdEXT = gpa("glImportSemaphoreFdEXT"); gSignalSemaphoreEXT = gpa("glSignalSemaphoreEXT");
    return 1;
}
static int interop_gl(void) { return gGetUnsignedBytevEXT && gCreateMemoryObjectsEXT && gImportMemoryFdEXT && gBufferStorageMemEXT && gGenSemaphoresEXT && gImportSemaphoreFdEXT && gSignalSemaphoreEXT; }

static GLuint shader(GLenum t, const char *s) { GLuint x = gCreateShader(t); gShaderSource(x, 1, &s, NULL); gCompileShader(x); return x; }

static int has_dev_ext(const char *n) {
    uint32_t c = 0; vkEnumerateDeviceExtensionProperties(V.pd, NULL, &c, NULL);
    VkExtensionProperties *e = malloc(sizeof *e * (c ? c : 1)); vkEnumerateDeviceExtensionProperties(V.pd, NULL, &c, e);
    int f = 0; for (uint32_t i = 0; i < c; i++) if (!strcmp(e[i].extensionName, n)) f = 1;
    free(e); return f;
}

static int make_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.pd, V.surf, &caps);
    VkExtent2D e = caps.currentExtent;
    if (e.width == 0xFFFFFFFF) { int sw, sh; ravengl_screen_size(&sw, &sh); e.width = sw; e.height = sh; }
    if (!e.width || !e.height) return 0;
    uint32_t n = caps.minImageCount + 1; if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkSwapchainKHR old = V.sc;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = V.surf, .minImageCount = n,
        .imageFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32, .imageColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT, .imageExtent = e,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = V.pm, .clipped = 1, .oldSwapchain = old };
    if (vkCreateSwapchainKHR(V.dev, &ci, NULL, &V.sc)) { V.sc = 0; return 0; }
    if (old) vkDestroySwapchainKHR(V.dev, old, NULL);
    V.nimg = 8; vkGetSwapchainImagesKHR(V.dev, V.sc, &V.nimg, V.img); V.ext = e;
    if (vkSetHdrMetadataEXT) {
        VkHdrMetadataEXT md = { VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
            .displayPrimaryRed = {0.708f, 0.292f}, .displayPrimaryGreen = {0.170f, 0.797f}, .displayPrimaryBlue = {0.131f, 0.046f},
            .whitePoint = {0.3127f, 0.3290f}, .maxLuminance = peak, .minLuminance = 0.005f,
            .maxContentLightLevel = peak, .maxFrameAverageLightLevel = paper };
        vkSetHdrMetadataEXT(V.dev, 1, &V.sc, &md);
    }
    fprintf(stderr, "[hdr] HDR10 swapchain %ux%u, %u images, %s\n", e.width, e.height, V.nimg,
            V.pm == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : V.pm == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    return 1;
}

// shared buffer (Vulkan memory, imported into GL) for one W x H frame of A2B10G10R10 pixels
static int make_buffer(int w, int h) {
    VkDeviceSize n = (VkDeviceSize)w * h * 4;
    VkExternalMemoryBufferCreateInfo eb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (vkCreateBuffer(V.dev, &bi, NULL, &V.buf)) return 0;
    VkMemoryRequirements q; vkGetBufferMemoryRequirements(V.dev, V.buf, &q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(V.pd, &mp);
    int t = -1; for (uint32_t i = 0; i < mp.memoryTypeCount && t < 0; i++)
        if ((q.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) t = i;
    if (t < 0) return 0;
    VkMemoryDedicatedAllocateInfo dd = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = V.buf };
    VkExportMemoryAllocateInfo ex = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dd, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, .allocationSize = q.size, .memoryTypeIndex = t };
    if (vkAllocateMemory(V.dev, &ai, NULL, &V.mem) || vkBindBufferMemory(V.dev, V.buf, V.mem, 0)) return 0;
    V.memsz = q.size;
    int fd = -1; VkMemoryGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = V.mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    if (vkGetMemoryFdKHR(V.dev, &gi, &fd)) return 0;
    gCreateMemoryObjectsEXT(1, &glmem); gImportMemoryFdEXT(glmem, V.memsz, 0x9586, fd);   // GL owns the fd now
    gGenBuffers(1, &glbuf); gBindBuffer(0x88EB, glbuf); gBufferStorageMemEXT(0x88EB, (intptr_t)n, glmem, 0); gBindBuffer(0x88EB, 0);
    // GL textures: srct = copy of the game's screen (RGBA16F), pqtex = HDR10-encoded (RGB10_A2)
    GLint ot; gGetIntegerv(0x8069, &ot);
    gGenTextures(1, &srct); gBindTexture(0x0DE1, srct); gTexImage2D(0x0DE1, 0, 0x881A, w, h, 0, 0x1908, 0x140B, NULL);
    gTexParameteri(0x0DE1, 0x2801, 0x2600); gTexParameteri(0x0DE1, 0x2800, 0x2600); gTexParameteri(0x0DE1, 0x813D, 0);
    gGenTextures(1, &pqtex); gBindTexture(0x0DE1, pqtex); gTexImage2D(0x0DE1, 0, 0x8059, w, h, 0, 0x1908, 0x8368, NULL);
    gTexParameteri(0x0DE1, 0x2801, 0x2600); gTexParameteri(0x0DE1, 0x2800, 0x2600); gTexParameteri(0x0DE1, 0x813D, 0);
    gBindTexture(0x0DE1, ot);
    W = w; H = h;
    return 1;
}

static int start(void) {
    if (!load_gl()) return 0;
    if (!interop_gl()) { off("the GL driver has no GL_EXT_memory_object_fd / GL_EXT_semaphore_fd"); return 0; }
    void *dpy = gXGetCurrentDisplay(); unsigned long win = gXGetCurrentDrawable();
    if (!dpy || !win) { off("no current GLX window"); return 0; }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib || !(gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr"))) { off("no Vulkan (libvulkan.so.1)"); return 0; }
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    const char *iext[] = {"VK_KHR_surface", "VK_KHR_xlib_surface", "VK_EXT_swapchain_colorspace"};
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "ravenhdr", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 3, .ppEnabledExtensionNames = iext };
    if (!vkCreateInstance || vkCreateInstance(&ici, NULL, &V.inst)) { off("Vulkan has no VK_EXT_swapchain_colorspace"); return 0; }
#define L(n) if (!(n = (PFN_##n)gipa(V.inst, #n))) { off("missing " #n); return 0; }
    VF(L)
#undef L
    VkXlibSurfaceCreateInfoKHR xi = { VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, .dpy = dpy, .window = win };
    if (vkCreateXlibSurfaceKHR(V.inst, &xi, NULL, &V.surf)) { off("vkCreateXlibSurfaceKHR failed"); return 0; }
    // the GPU GL runs on (UUID match), with an HDR10 format for this window
    uint8_t uuid[16] = {0}; gGetUnsignedBytevEXT(0x9597, uuid);   // GL_DEVICE_UUID_EXT
    uint32_t n = 16; VkPhysicalDevice pds[16]; vkEnumeratePhysicalDevices(V.inst, &n, pds);
    V.pd = 0;
    for (uint32_t i = 0; i < n && !V.pd; i++) {
        VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
        vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (!memcmp(idp.deviceUUID, uuid, 16)) V.pd = pds[i];
    }
    if (!V.pd) { off("no Vulkan device matches the GL GPU"); return 0; }
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, V.surf, &nf, NULL);
    VkSurfaceFormatKHR *fm = malloc(sizeof *fm * (nf ? nf : 1)); vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, V.surf, &nf, fm);
    int ok = 0; for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && fm[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) ok = 1;
    free(fm);
    if (!ok) {
        off(getenv("GAMESCOPE_WAYLAND_DISPLAY") ? "gamescope offers no HDR10 format (start it with --hdr-enabled, and turn HDR on for the display)"
                                                : "this X11 window has no HDR10 format: HDR needs gamescope --hdr-enabled (play_halo.sh starts it when HDR = 1 and gamescope is installed)");
        return 0;
    }
    uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, qp);
    V.qf = nq;
    for (uint32_t i = 0; i < nq && V.qf == nq; i++) { VkBool32 sup = 0; vkGetPhysicalDeviceSurfaceSupportKHR(V.pd, i, V.surf, &sup); if (sup) V.qf = i; }
    if (V.qf == nq) { off("no present queue"); return 0; }
    const char *dext[5] = {"VK_KHR_swapchain", "VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd"}; int ne = 3;
    for (int i = 0; i < 3; i++) if (!has_dev_ext(dext[i])) { off("the Vulkan driver lacks swapchain / external memory / semaphore support"); return 0; }
    int md = has_dev_ext("VK_EXT_hdr_metadata"); if (md) dext[ne++] = "VK_EXT_hdr_metadata";
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = ne, .ppEnabledExtensionNames = dext };
    if (vkCreateDevice(V.pd, &dci, NULL, &V.dev)) { off("vkCreateDevice failed"); return 0; }
#define L(n) if (!(n = (PFN_##n)vkGetDeviceProcAddr(V.dev, #n))) { off("missing " #n); return 0; }
    DF(L)
#undef L
    if (md) vkSetHdrMetadataEXT = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(V.dev, "vkSetHdrMetadataEXT");
    vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
    // VSync: FIFO; off: mailbox if there is one, else immediate
    uint32_t np = 8; VkPresentModeKHR pms[8]; vkGetPhysicalDeviceSurfacePresentModesKHR(V.pd, V.surf, &np, pms);
    V.pm = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) for (uint32_t i = 0; i < np; i++) { if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR) V.pm = pms[i]; else if (pms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && V.pm == VK_PRESENT_MODE_FIFO_KHR) V.pm = pms[i]; }
    if (!make_swapchain()) { off("vkCreateSwapchainKHR failed"); return 0; }
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandBufferCount = 1 };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkExportSemaphoreCreateInfo es = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkSemaphoreCreateInfo sce = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &es };
    if (vkCreateCommandPool(V.dev, &pci, NULL, &V.pool)) { off("command pool"); return 0; }
    cai.commandPool = V.pool;
    if (vkAllocateCommandBuffers(V.dev, &cai, &V.cb) || vkCreateFence(V.dev, &fci, NULL, &V.fence) ||
        vkCreateSemaphore(V.dev, &sce, NULL, &V.sem_gl) || vkCreateSemaphore(V.dev, &sci, NULL, &V.sem_acq) || vkCreateSemaphore(V.dev, &sci, NULL, &V.sem_done)) { off("sync objects"); return 0; }
    int sfd = -1; VkSemaphoreGetFdInfoKHR sg = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = V.sem_gl, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
    if (vkGetSemaphoreFdKHR(V.dev, &sg, &sfd)) { off("semaphore export"); return 0; }
    gGenSemaphoresEXT(1, &glsem); gImportSemaphoreFdEXT(glsem, 0x9586, sfd);
    if (!make_buffer(V.ext.width, V.ext.height)) { off("shared buffer"); return 0; }
    // GL program
    GLuint v = shader(0x8B31, vs), f = shader(0x8B30, fs);
    prog = gCreateProgram(); gAttachShader(prog, v); gAttachShader(prog, f); gLinkProgram(prog);
    GLint lk = 0; gGetProgramiv(prog, 0x8B82, &lk);
    if (!lk) { char log[1024]; gGetProgramInfoLog(prog, sizeof log, NULL, log); fprintf(stderr, "[hdr] shader: %s\n", log); off("shader"); return 0; }
    GLint ova; gGetIntegerv(0x85B5, &ova); gGenVertexArrays(1, &vao); gBindVertexArray(ova);
    gGenFramebuffers(1, &fb_src); gGenFramebuffers(1, &fb_dst);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(V.pd, &pp);
    fprintf(stderr, "[hdr] native HDR10 output on %s (paper white %.0f nits, peak %.0f nits%s)\n", pp.deviceName, paper, peak, md ? ", HDR metadata" : "");
    return 1;
}

// GL: the game's screen -> HDR10 pixels in the shared buffer, then signal Vulkan.
static void encode(void) {
    GLint rf, df, pr, va, at, t0, vp[4], rb, pk, pa, prl;
    gGetIntegerv(0x8CAA, &rf); gGetIntegerv(0x8CA6, &df); gGetIntegerv(0x8B8D, &pr); gGetIntegerv(0x85B5, &va); gGetIntegerv(0x84E0, &at);
    gGetIntegerv(0x0BA2, vp); gGetIntegerv(0x88ED, &pk); gGetIntegerv(0x0D05, &pa); gGetIntegerv(0x0D02, &prl);
    static const GLenum caps[] = {0x0C11, 0x0BE2, 0x0B71, 0x0B44, 0x0B90, 0x8DB9, 0x8C89};
    GLboolean en[7]; for (int i = 0; i < 7; i++) { en[i] = gIsEnabled(caps[i]); gDisable(caps[i]); }
    unsigned sfb = ravengl_screen_fbo(); int sw = W, sh = H; if (sfb) ravengl_screen_size(&sw, &sh);
    gBindFramebuffer(0x8CA8, sfb); gGetIntegerv(0x0C02, &rb); gReadBuffer(sfb ? 0x8CE0 : 0x0405);
    gBindFramebuffer(0x8CA9, fb_src); gFramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, srct, 0);
    gBlitFramebuffer(0, 0, sw, sh, 0, 0, W, H, 0x4000, sw == W && sh == H ? 0x2600 : 0x2601);
    gReadBuffer(rb);
    // graphics.ini HDR_STATS = 1: every 300 frames, the brightest pixel and how many are above SDR white
    static int fr, stats = -1;
    if (stats < 0) { const char *v = ravengfx_get("HDR_STATS"); stats = v && atoi(v) > 0; }
    if (stats && ++fr % 300 == 0 && W <= 8192) {
        static float *row; if (!row) row = malloc(sizeof(float) * 4 * 8192);
        float mx = 0; long over = 0, tot = 0;
        gBindFramebuffer(0x8CA8, fb_src); gReadBuffer(0x8CE0); gBindBuffer(0x88EB, 0);
        for (int y = 0; y < 64; y++) {
            gReadPixels(0, H * y / 64 + H / 128, W, 1, 0x1908, 0x1406, row);
            for (int x = 0; x < W; x++) { float m = fmaxf(row[x * 4], fmaxf(row[x * 4 + 1], row[x * 4 + 2])); if (m > mx) mx = m; if (m > 1.001f) over++; tot++; }
        }
        fprintf(stderr, "[hdr] frame %d: brightest %.2fx white (~%.0f nits), %.2f%% of pixels above SDR white\n",
                fr, mx, powf(mx, 2.2f) * paper, tot ? 100.0 * over / tot : 0.0);
    }
    gBindFramebuffer(0x8CA9, fb_dst); gFramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, pqtex, 0); gDrawBuffer(0x8CE0);
    gViewport(0, 0, W, H); gColorMask(1, 1, 1, 1);
    gUseProgram(prog); gBindVertexArray(vao);
    gActiveTexture(0x84C0); gGetIntegerv(0x8069, &t0); gBindSampler(0, 0); gBindTexture(0x0DE1, srct);
    gUniform1i(gGetUniformLocation(prog, "S"), 0); gUniform1i(gGetUniformLocation(prog, "h"), H);
    gUniform1f(gGetUniformLocation(prog, "paper"), paper); gUniform1f(gGetUniformLocation(prog, "peak"), peak);
    gDrawArrays(0x0004, 0, 3);
    // pqtex -> shared buffer as packed 2_10_10_10 (= VK_FORMAT_A2B10G10R10_UNORM_PACK32)
    gBindTexture(0x0DE1, pqtex); gBindBuffer(0x88EB, glbuf); gPixelStorei(0x0D05, 4); gPixelStorei(0x0D02, 0);
    gGetTexImage(0x0DE1, 0, 0x1908, 0x8368, NULL);
    gBindBuffer(0x88EB, pk);
    GLenum lay = 0x958D;   // GL_LAYOUT_TRANSFER_SRC_EXT (unused for buffers)
    gSignalSemaphoreEXT(glsem, 1, &glbuf, 0, NULL, &lay);
    gFlush();
    gBindTexture(0x0DE1, t0); gActiveTexture(at); gBindVertexArray(va); gUseProgram(pr);
    gBindFramebuffer(0x8CA8, rf); gBindFramebuffer(0x8CA9, df); gPixelStorei(0x0D05, pa); gPixelStorei(0x0D02, prl);
    gViewport(vp[0], vp[1], vp[2], vp[3]);
    for (int i = 0; i < 7; i++) if (en[i]) gEnable(caps[i]);
}

static void barrier(VkImage im, VkImageLayout a, VkImageLayout b, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = a, .newLayout = b,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1} };
    vkCmdPipelineBarrier(V.cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &ib);
}

static int present(void) {
    if (V.fence_pending) { vkWaitForFences(V.dev, 1, &V.fence, 1, UINT64_MAX); vkResetFences(V.dev, 1, &V.fence); V.fence_pending = 0; }
    uint32_t idx;
    VkResult r = vkAcquireNextImageKHR(V.dev, V.sc, UINT64_MAX, V.sem_acq, 0, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(V.dev); if (!make_swapchain()) { off("swapchain lost"); return 0; } return 1; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { off("vkAcquireNextImageKHR failed"); return 0; }
    encode();
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(V.cb, 0); vkBeginCommandBuffer(V.cb, &bi);
    barrier(V.img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    uint32_t cw = V.ext.width < (uint32_t)W ? V.ext.width : (uint32_t)W, ch = V.ext.height < (uint32_t)H ? V.ext.height : (uint32_t)H;
    VkBufferImageCopy c = { .bufferRowLength = W, .bufferImageHeight = H, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {cw, ch, 1} };
    vkCmdCopyBufferToImage(V.cb, V.buf, V.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(V.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(V.cb);
    VkSemaphore w[2] = {V.sem_gl, V.sem_acq}; VkPipelineStageFlags ws[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 2, .pWaitSemaphores = w, .pWaitDstStageMask = ws,
        .commandBufferCount = 1, .pCommandBuffers = &V.cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &V.sem_done };
    if (vkQueueSubmit(V.q, 1, &si, V.fence)) { off("vkQueueSubmit failed"); return 0; }
    V.fence_pending = 1;
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &V.sem_done,
        .swapchainCount = 1, .pSwapchains = &V.sc, .pImageIndices = &idx };
    r = vkQueuePresentKHR(V.q, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(V.dev); V.fence_pending = 0; vkResetFences(V.dev, 1, &V.fence); make_swapchain(); }
    return 1;
}

// Called instead of the game's swap (real and generated frames). 1 = presented through Vulkan HDR10;
// 0 = the caller swaps as before (the screen framebuffer was copied to the window first).
int ravenhdr_present(void) {
    if (!ravenhdr_on()) return 0;
    if (!st) { read_settings(); st = start() ? 1 : -1; }
    if (st > 0 && present()) return 1;
    // SDR fallback: the game drew into our float screen framebuffer, show it on the window
    unsigned sfb = ravengl_screen_fbo();
    if (sfb && gBlitFramebuffer) {
        GLint rf, df, sc_on = gIsEnabled(0x0C11); int sw, sh;
        gGetIntegerv(0x8CAA, &rf); gGetIntegerv(0x8CA6, &df);
        ravengl_screen_size(&sw, &sh); gDisable(0x0C11);
        gBindFramebuffer(0x8CA8, sfb); gBindFramebuffer(0x8CA9, 0); gDrawBuffer(0x0405);
        gBlitFramebuffer(0, 0, sw, sh, 0, 0, sw, sh, 0x4000, 0x2600);
        gBindFramebuffer(0x8CA8, rf); gBindFramebuffer(0x8CA9, df);
        if (sc_on) gEnable(0x0C11);
    }
    return 0;
}
