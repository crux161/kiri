--[[
  kiri.lua — LuaJIT FFI binding for libkiri.

  Usage:  luajit kiri.lua <input> <output_dir> [--playlist]

  Requires LuaJIT (not plain Lua). For plain Lua use a C binding module.
]]

local ffi = require("ffi")

-- C declarations mirroring kiri.h. Keep in sync across major versions.
ffi.cdef[[
typedef enum {
    KIRI_OK = 0,
    KIRI_ERR_INVALID_ARG  = -1,
    KIRI_ERR_OPEN_INPUT   = -2,
    KIRI_ERR_STREAM_INFO  = -3,
    KIRI_ERR_NO_STREAMS   = -4,
    KIRI_ERR_OPEN_OUTPUT  = -5,
    KIRI_ERR_WRITE        = -6,
    KIRI_ERR_ALLOC        = -7,
    KIRI_ERR_MKDIR        = -8,
    KIRI_ERR_DISK_SPACE   = -9,
    KIRI_ERR_INTERRUPTED  = -10,
    KIRI_ERR_IO           = -11,
    KIRI_ERR_FFMPEG       = -100
} kiri_status;

typedef int  (*kiri_progress_fn)(void *user, int seg, int total,
                                 int64_t done, int64_t total_bytes);
typedef void (*kiri_log_fn)(void *user, int level, const char *msg);

typedef struct {
    int64_t size_bytes;
    int64_t duration_us;
    int     nb_streams;
    int     has_video;
    int     video_width;
    int     video_height;
    char    format_long_name[128];
    char    video_codec_name[32];
} kiri_input_info;

typedef struct {
    const char *input_path;
    const char *output_dir;
    int64_t     max_bytes;
    const char *basename;
    const char *extension;
    int64_t     safety_margin_bytes;
    int64_t     preallocate_bytes;
    int         make_playlist;
    kiri_progress_fn on_progress;
    kiri_log_fn      on_log;
    void            *user_data;
} kiri_split_options;

const char *kiri_version_string(void);
const char *kiri_strerror(kiri_status status);
int64_t     kiri_fat32_max_bytes(void);
int64_t     kiri_chunk_count(int64_t file_size_bytes, int64_t chunk_size_bytes);
kiri_status kiri_probe(const char *input_path, kiri_input_info *info);
kiri_status kiri_split(const kiri_split_options *options);
]]

-- Locate shared library next to this file, then fall back to loader path.
local function load_kiri()
    local names
    if ffi.os == "OSX" then
        names = { "libkiri.dylib" }
    elseif ffi.os == "Windows" then
        names = { "kiri.dll", "libkiri.dll" }
    else
        names = { "libkiri.so" }
    end

    local info = debug.getinfo(1, "S").source:sub(2)
    local here = info:match("(.*/)") or "./"
    local roots = { here, here .. "../" }

    for _, name in ipairs(names) do
        for _, root in ipairs(roots) do
            local path = root .. name
            local ok, lib = pcall(ffi.load, path)
            if ok then return lib end
        end
        local ok, lib = pcall(ffi.load, name)
        if ok then return lib end
    end
    error("could not load libkiri")
end

local C = load_kiri()

local M = {}
M.OK               =   0
M.ERR_INTERRUPTED  = -10
M.LOG_ERROR   = 0
M.LOG_WARN    = 1
M.LOG_INFO    = 2
M.LOG_VERBOSE = 3

function M.version()  return ffi.string(C.kiri_version_string()) end
function M.strerror(code) return ffi.string(C.kiri_strerror(code)) end
function M.fat32_max() return tonumber(C.kiri_fat32_max_bytes()) end

function M.probe(path)
    local info = ffi.new("kiri_input_info")
    local rc = C.kiri_probe(path, info)
    if rc ~= M.OK then error("probe failed: " .. M.strerror(rc)) end
    return {
        size_bytes   = tonumber(info.size_bytes),
        duration_us  = tonumber(info.duration_us),
        nb_streams   = info.nb_streams,
        has_video    = info.has_video ~= 0,
        video_width  = info.video_width,
        video_height = info.video_height,
        format       = ffi.string(info.format_long_name),
        video_codec  = ffi.string(info.video_codec_name),
    }
end

-- split(opts) where opts is a Lua table with keys:
--   input_path, output_dir, max_bytes (required)
--   make_playlist, basename, extension (optional)
--   on_progress(seg, total, done, total_bytes) -> boolean (true = abort)
--   on_log(level, msg)
function M.split(opts)
    assert(opts.input_path and opts.output_dir and opts.max_bytes,
           "input_path, output_dir, max_bytes required")

    -- Keep callbacks pinned so the JIT doesn't collect them mid-call
    local progress_cb = ffi.cast("kiri_progress_fn",
        function(_, seg, total, done, tot)
            if not opts.on_progress then return 0 end
            local ok, abort = pcall(opts.on_progress,
                seg, total, tonumber(done), tonumber(tot))
            if not ok or abort then return 1 end
            return 0
        end)

    local log_cb = ffi.cast("kiri_log_fn",
        function(_, level, msg)
            if opts.on_log then
                pcall(opts.on_log, level, ffi.string(msg))
            end
        end)

    local c_opts = ffi.new("kiri_split_options", {
        input_path        = opts.input_path,
        output_dir        = opts.output_dir,
        max_bytes         = opts.max_bytes,
        basename          = opts.basename,
        extension         = opts.extension,
        safety_margin_bytes = opts.safety_margin_bytes or 0,
        preallocate_bytes = opts.preallocate_bytes or -1,
        make_playlist     = opts.make_playlist and 1 or 0,
        on_progress       = progress_cb,
        on_log            = log_cb,
        user_data         = nil,
    })

    local rc = C.kiri_split(c_opts)
    progress_cb:free()
    log_cb:free()
    if rc ~= M.OK then
        error("kiri_split failed: " .. M.strerror(rc), 2)
    end
end

-- --- Demo -----------------------------------------------------------------
if arg and arg[0] and arg[0]:match("kiri%.lua$") then
    if #arg < 2 then
        io.stderr:write(string.format("Usage: %s <input> <output_dir> [--playlist]\n", arg[0]))
        os.exit(1)
    end

    local input_path   = arg[1]
    local output_dir   = arg[2]
    local make_playlist = false
    for i = 3, #arg do
        if arg[i] == "--playlist" then make_playlist = true end
    end

    os.execute(string.format("mkdir -p %q", output_dir))

    print(string.format("libkiri %s", M.version()))
    local info = M.probe(input_path)
    print(string.format(
        "Input: %s  %.1f MiB  %.1f s  %s %dx%d",
        info.format,
        info.size_bytes / 1048576,
        info.duration_us / 1e6,
        info.video_codec, info.video_width, info.video_height))

    M.split{
        input_path    = input_path,
        output_dir    = output_dir,
        max_bytes     = 200 * 1024 * 1024,
        make_playlist = make_playlist,
        on_progress = function(seg, total, done, tot)
            local pct = tot > 0 and (100 * done / tot) or 0
            io.write(string.format("\r segment %d/%d  [%.1f%%]", seg, total, pct))
            io.flush()
            return false
        end,
        on_log = function(level, msg)
            local tags = { [0] = "error", [1] = "warn", [2] = "info", [3] = "debug" }
            io.stderr:write(string.format("\n[%s] %s\n", tags[level] or "?", msg))
        end,
    }
    print("\nDone.")
end

return M
