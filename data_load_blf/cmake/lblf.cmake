# lblf.cmake -- the patched lblf (PetStr/lblf) BLF reader as a static library.
# Shared by this plugin and PlotJuggler's WebAssembly build: each fetches the
# same pinned source and calls pj_add_lblf_library(<target> <lblf source dir>),
# so the hardening below lives in one place. Needs ZLIB::ZLIB.
# Only its two library sources are built (blf_reader.cc + print.cc):
# object_types.cc and the read*.cc files are example programs with a main().
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/VendoredPatch.cmake)

function(pj_add_lblf_library target source_dir)
# Vendored hardening: the pinned lblf computes a log container's blob size as
# `objSize - headerSize - sizeof(LogContainer)`. A forged objSize below that
# sum underflows to ~4 GB; a forged objSize far above the file makes the same
# math (or unCompressedFileSize) resize() a multi-GB buffer before the short
# read fails — an allocation DoS from a tiny file either way. Inject a bounds
# check right after the container header is read: reject objSize outside
# [header, fileLength]. `fileLength` is the reader's own member, set from the
# real file size in the constructor. Done via string(REPLACE) (no `patch`
# executable needed on Windows CI); each pj_apply_vendored_patch FATAL_ERRORs if
# its anchor moves, so a pin bump can never silently drop a guard.
file(READ ${source_dir}/blf_reader.cc _lblf_reader_cc)

set(_lblf_anchor [==[            read_logcontainer(fileStream, lc, ohb);]==])
set(_lblf_fixed [==[            if (ohb.objSize < ohb.headerSize + sizeof(blf_struct::LogContainer) || ohb.objSize > fileLength)
                {
                    throw std::runtime_error("lblf: log container size out of range (corrupt file)");
                }
            read_logcontainer(fileStream, lc, ohb);]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_anchor _lblf_fixed "data_load_blf: lblf log-container size")

# Second guard: the zlib output buffer is sized from the container's own
# unCompressedFileSize field, which is independent of objSize. Cap it against
# the compressed blob size (zlib's theoretical max expansion is ~1032:1) so a
# forged value cannot resize() a multi-GB buffer.
set(_lblf_zlib_anchor [==[                    uLong uncompressedFileSize = lc.unCompressedFileSize;
                    std::vector<uint8_t> uncompressedData {};

                    uncompressedData.resize(uncompressedFileSize);]==])
set(_lblf_zlib_fixed [==[                    uLong uncompressedFileSize = lc.unCompressedFileSize;
                    if (uncompressedFileSize > static_cast<uLong>(compressedBlobSize) * 1100 + 65536)
                        {
                            throw std::runtime_error("lblf: implausible uncompressed container size (corrupt file)");
                        }
                    std::vector<uint8_t> uncompressedData {};

                    uncompressedData.resize(uncompressedFileSize);]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_zlib_anchor _lblf_zlib_fixed "data_load_blf: lblf zlib output size")

# Third guard: blf_reader::data() computes an inner object's payload length as
# `objSize - sizeof(BaseHeader) + objSize % 4`. A forged inner objSize below
# sizeof(BaseHeader) underflows that size_t, and the following pop loop then
# reads past an emptied deque (front()/pop_front() on empty = undefined). Bail
# to the end-of-stream sentinel (0x888, already used above) when the object is
# undersized or the stream ran short.
set(_lblf_data_anchor [==[    if (logcontainer_que.size() < size_of_data)
        {
            fill_deque();
        }

    result.payload.clear();
    result.payload.reserve(size_of_data);]==])
set(_lblf_data_fixed [==[    if (logcontainer_que.size() < size_of_data)
        {
            fill_deque();
        }

    if (result.base_header.objSize < sizeof(result.base_header) || logcontainer_que.size() < size_of_data)
        {
            result.base_header.objectType = static_cast<ObjectType_e>(0x888);
            return result;
        }

    result.payload.clear();
    result.payload.reserve(size_of_data);]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_data_anchor _lblf_data_fixed "data_load_blf: lblf inner-object size")

# 64-bit file positions: the pinned lblf keeps the file length and read position in 32 bits, so a file of 4 GiB or
# more ends its read at the size modulo 4 GiB (silently dropping the rest), and skipping a top-level object over
# 2 GiB seeks backwards. The length member lives in the header, so the patched header shadows the original for
# every consumer (blf_reader.cc finds it beside itself; the include path puts it first).
set(_lblf_length_fn_anchor [==[    auto getfileLength(std::fstream &fileStream) -> int32_t
    {
        fileStream.seekg(0, std::fstream::end);
        const auto length = static_cast<int32_t>(fileStream.tellg());]==])
set(_lblf_length_fn_fixed [==[    auto getfileLength(std::fstream &fileStream) -> int64_t
    {
        fileStream.seekg(0, std::fstream::end);
        const auto length = static_cast<int64_t>(fileStream.tellg());]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_length_fn_anchor _lblf_length_fn_fixed "data_load_blf: lblf 64-bit file length")
set(_lblf_length_anchor [==[            fileLength = static_cast<uint32_t>(getfileLength(fileStream));]==])
set(_lblf_length_fixed [==[            fileLength = static_cast<uint64_t>(getfileLength(fileStream));]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_length_anchor _lblf_length_fixed "data_load_blf: lblf 64-bit file length member")
set(_lblf_position_anchor [==[    auto current_position = static_cast<uint32_t>(fileStream.tellg());]==])
set(_lblf_position_fixed [==[    auto current_position = static_cast<uint64_t>(fileStream.tellg());]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_position_anchor _lblf_position_fixed "data_load_blf: lblf 64-bit read position")
set(_lblf_jump_anchor [==[            const auto bytes_to_jump = static_cast<int32_t>(ohb.objSize - ohb.headerSize + (ohb.objSize % 4));]==])
set(_lblf_jump_fixed [==[            const auto bytes_to_jump = static_cast<std::streamoff>(ohb.objSize) - ohb.headerSize + (ohb.objSize % 4);]==])
pj_apply_vendored_patch(_lblf_reader_cc _lblf_jump_anchor _lblf_jump_fixed "data_load_blf: lblf 64-bit object skip")

file(READ ${source_dir}/blf_reader.hh _lblf_reader_hh)
set(_lblf_member_anchor [==[    uint32_t fileLength {0};]==])
set(_lblf_member_fixed [==[    uint64_t fileLength {0};]==])
pj_apply_vendored_patch(_lblf_reader_hh _lblf_member_anchor _lblf_member_fixed "data_load_blf: lblf 64-bit file length header")

set(_lblf_patched_dir ${CMAKE_CURRENT_BINARY_DIR}/${target}_patched)
file(CONFIGURE OUTPUT ${_lblf_patched_dir}/blf_reader.cc CONTENT "${_lblf_reader_cc}" @ONLY)
file(CONFIGURE OUTPUT ${_lblf_patched_dir}/blf_reader.hh CONTENT "${_lblf_reader_hh}" @ONLY)

add_library(${target} STATIC
  ${_lblf_patched_dir}/blf_reader.cc
  ${source_dir}/print.cc
)
set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(${target} PRIVATE cxx_std_20)
# SYSTEM so lblf's headers don't trip the plugin's -Werror.
target_include_directories(${target} SYSTEM BEFORE PUBLIC ${_lblf_patched_dir} ${source_dir})
target_link_libraries(${target} PRIVATE ZLIB::ZLIB)
endfunction()
