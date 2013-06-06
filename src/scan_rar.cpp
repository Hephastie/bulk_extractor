#include "config.h"
#include "bulk_extractor_i.h"

#include "utf8.h"
#include "dfxml/src/dfxml_writer.h"

#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <iomanip>
#include <cassert>

#include "rar/rar.hpp"

#define FILE_MAGIC 0x74
#define FILE_HEAD_MIN_LEN 32
#define ARCHIVE_MAGIC 0x73
#define ARCHIVE_HEAD_MIN_LEN 13

#define OFFSET_HEAD_CRC 0
#define OFFSET_HEAD_TYPE 2
#define OFFSET_HEAD_FLAGS 3
#define OFFSET_HEAD_SIZE 5
#define OFFSET_PACK_SIZE 7
#define OFFSET_UNP_SIZE 11
#define OFFSET_HOST_OS 15
#define OFFSET_FILE_CRC 16
#define OFFSET_FTIME 20
#define OFFSET_UNP_VER 24
#define OFFSET_METHOD 25
#define OFFSET_NAME_SIZE 26
#define OFFSET_ATTR 28
#define OFFSET_HIGH_PACK_SIZE 32
#define OFFSET_HIGH_UNP_SIZE 36
#define OFFSET_FILE_NAME 32
#define OFFSET_SALT 32
#define OFFSET_EXT_TIME 40

#define MANDATORY_FILE_FLAGS 0x8000
#define UNUSED_FILE_FLAGS 0x6000
#define UNUSED_ARCHIVE_FLAGS 0xFE00

#define FLAG_CONT_PREV 0x0001
#define FLAG_CONT_NEXT 0x0002
#define FLAG_ENCRYPTED 0x0004
#define FLAG_COMMENT 0x0008
#define FLAG_SOLID 0x0010
#define MASK_DICT 0x00E0
#define FLAG_BIGFILE 0x0100
#define FLAG_UNICODE_FILENAME 0x0200
#define FLAG_SALTED 0x0400
#define FLAG_OLD_VER 0x0800
#define FLAG_EXTIME 0x1000
#define FLAG_HEADERS_ENCRYPTED 0x0080

#define OS_DOS 0x00
#define OS_OS2 0x01
#define OS_WINDOWS 0x02
#define OS_UNIX 0x03
#define OS_MAC 0x04
#define OS_BEOS 0x05

#define METHOD_UNCOMPRESSED 0x30
#define METHOD_FASTEST 0x31
#define METHOD_FAST 0x32
#define METHOD_NORMAL 0x33
#define METHOD_SMALL 0x34
#define METHOD_SMALLEST 0x35

#define OPTIONAL_BIGFILE_LEN 8

#define SUSPICIOUS_HEADER_LEN 1024
#define SUSPICIOUS_FILE_LEN 10L * 1024L * 1024L * 1024L * 1024L

#define STRING_BUF_LEN 1024

using namespace std;

static inline int int2(const u_char *cc)
{
    return (cc[1]<<8) + cc[0];
}

static inline int int4(const u_char *cc)
{
    return (cc[3]<<24) + (cc[2]<<16) + (cc[1]<<8) + (cc[0]);
}

#define DOS_MASK_SECOND 0x0000001F
#define DOS_SHIFT_SECOND 0
#define DOS_MASK_MINUTE 0x000007E0
#define DOS_SHIFT_MINUTE 5
#define DOS_MASK_HOUR 0x0000F800
#define DOS_SHIFT_HOUR 11
#define DOS_MASK_DAY 0x001F0000
#define DOS_SHIFT_DAY 16
#define DOS_MASK_MONTH 0x01E00000
#define DOS_SHIFT_MONTH 21
#define DOS_MASK_YEAR 0xFE000000
#define DOS_SHIFT_YEAR 25
#define DOS_OFFSET_YEAR 1980

static string dos_date_to_iso(uint32_t dos_date) {
    uint8_t seconds = (dos_date & DOS_MASK_SECOND) >> DOS_SHIFT_SECOND;
    uint8_t minutes = (dos_date & DOS_MASK_MINUTE) >> DOS_SHIFT_MINUTE;
    uint8_t hours = (dos_date & DOS_MASK_HOUR) >> DOS_SHIFT_HOUR;
    uint8_t days = (dos_date & DOS_MASK_DAY) >> DOS_SHIFT_DAY;
    uint8_t months = (dos_date & DOS_MASK_MONTH) >> DOS_SHIFT_MONTH;
    uint16_t years = (dos_date & DOS_MASK_YEAR) >> DOS_SHIFT_YEAR;

    years += DOS_OFFSET_YEAR;
    seconds *= 2;

    char buf[STRING_BUF_LEN];
    snprintf(buf,sizeof(buf),"%04d-%02d-%02dT%02d:%02d:%02dZ",
            years, months, days, hours, minutes, seconds);
    stringstream ss;
    ss << buf;
    return ss.str();
}

//
// CRC32
//
// generated by pycrc 0.8
static inline uint32_t crc_init(void)
{
    return 0xffffffff;
}
static uint32_t crc_reflect(uint32_t data, size_t data_len)
{
    unsigned int i;
    uint32_t ret;

    ret = data & 0x01;
    for (i = 1; i < data_len; i++) {
        data >>= 1;
        ret = (ret << 1) | (data & 0x01);
    }
    return ret;
}
static inline uint32_t crc_finalize(uint32_t crc)
{
    return crc_reflect(crc, 32) ^ 0xffffffff;
}
static uint32_t crc_update(uint32_t crc, const unsigned char *data, size_t data_len)
{
    unsigned int i;
    bool bit;
    unsigned char c;

    while (data_len--) {
        c = *data++;
        for (i = 0x01; i & 0xff; i <<= 1) {
            bit = crc & 0x80000000;
            if (c & i) {
                bit = !bit;
            }
            crc <<= 1;
            if (bit) {
                crc ^= 0x04c11db7;
            }
        }
        crc &= 0xffffffff;
    }
    return crc & 0xffffffff;
}

//
// RAR processing
//

class RarComponentInfo {
public:
    explicit RarComponentInfo() :
        name(), flags(), unpack_version(), compression_method(),
        uncompressed_size(), compressed_size(), file_attributes(), dos_time(),
        host_os(), crc() {}
    explicit RarComponentInfo(const std::string &name_, const uint16_t flags_,
            const uint8_t unpack_version_, const uint8_t compression_method_,
            const uint64_t uncompressed_size_, const uint64_t compressed_size_,
            const uint32_t file_attributes_, const uint32_t dos_time_,
            const uint8_t host_os_, const uint32_t crc_) :
        name(name_), flags(flags_), unpack_version(unpack_version_),
        compression_method(compression_method_),
        uncompressed_size(uncompressed_size_),
        compressed_size(compressed_size_), file_attributes(file_attributes_),
        dos_time(dos_time_), host_os(host_os_), crc(crc_) {}

    inline const uint8_t unpack_version_major() const
    {
        return unpack_version / 10;
    }
    inline const uint8_t unpack_version_minor() const
    {
        return unpack_version % 10;
    }
    std::string to_xml() const;
    std::string compression_method_label() const;
    std::string host_os_label() const;

    std::string name;
    uint16_t flags;
    uint8_t unpack_version;
    uint8_t compression_method;
    uint64_t uncompressed_size;
    uint64_t compressed_size;
    uint32_t file_attributes;
    uint32_t dos_time;
    uint8_t host_os;
    uint32_t crc;
};

string RarComponentInfo::to_xml() const
{
    char string_buf[STRING_BUF_LEN];

    // build XML output
    string filename = dfxml_writer::xmlescape(name);

    snprintf(string_buf,sizeof(string_buf),
             "<rar_component>"
             "<name>%s</name>"
             "<flags>0x%04X</flags><version>%d</version><compression_method>%s</compression_method>"
             "<uncompr_size>%"PRIu64"</uncompr_size><compr_size>%"PRIu64"</compr_size><file_attr>0x%X</file_attr>"
             "<lastmoddate>%s</lastmoddate><host_os>%s</host_os><crc32>0x%08X</crc32>"
             "</rar_component>",
             name.c_str(), flags, unpack_version,
             compression_method_label().c_str(), uncompressed_size,
             compressed_size, file_attributes,
             dos_date_to_iso(dos_time).c_str(), host_os_label().c_str(), crc);

    return string(string_buf);
}

string RarComponentInfo::compression_method_label() const
{
    char string_buf[5];
    switch(compression_method) {
        case METHOD_UNCOMPRESSED:
            return "uncompressed";
        case METHOD_FASTEST:
            return "fastest";
        case METHOD_FAST:
            return "fast";
        case METHOD_NORMAL:
            return "normal";
        case METHOD_SMALL:
            return "small";
        case METHOD_SMALLEST:
            return "smallest";
        default:
            snprintf(string_buf, sizeof(string_buf), "0x%02X", compression_method);
            return string(string_buf);
    }
}

string RarComponentInfo::host_os_label() const
{
    char string_buf[5];
    switch(host_os) {
        case OS_DOS:
            return "DOS";
        case OS_OS2:
            return "OS/2";
        case OS_WINDOWS:
            return "Windows";
        case OS_UNIX:
            return "Unix";
        case OS_MAC:
            return "Mac OS";
        case OS_BEOS:
            return "BeOS";
        default:
            snprintf(string_buf, sizeof(string_buf), "0x%02X", host_os);
            return string(string_buf);
    }
}

class RarVolumeInfo {
public:
    explicit RarVolumeInfo() : flags() {}
    explicit RarVolumeInfo(uint16_t flags_) : flags(flags_) {}

    std::string to_xml() const;

    uint16_t flags;
};

string RarVolumeInfo::to_xml() const
{
    char string_buf[STRING_BUF_LEN];

    snprintf(string_buf, sizeof(string_buf),
             "<rar_volume>"
             "<encrypted>%s</encrypted>"
             "</rar_volume>",
             flags & FLAG_HEADERS_ENCRYPTED ? "true" : "false");

    return string(string_buf);
}

// settings - these configuration vars are set when the scanner is created
static bool record_components = true;
static bool record_volumes = true;

// component processing (compressed file within an archive)
static bool process_component(const unsigned char *buf, size_t buf_len, RarComponentInfo &output)
{
    // confirm that the smallest possible component block header could fit in
    // the buffer
    if(buf_len < FILE_HEAD_MIN_LEN) {
        return false;
    }
    // Initial RAR file block anchor is 0x74 magic byte
    if(buf[OFFSET_HEAD_TYPE] != FILE_MAGIC) {
        return false;
    }
    // check for invalid flags
    uint16_t flags = (uint16_t) int2(buf + OFFSET_HEAD_FLAGS);
    if(!(flags & MANDATORY_FILE_FLAGS) || (flags & UNUSED_FILE_FLAGS)) {
        return false;
    }
    // ignore split files and encrypted files
    if(flags & (FLAG_CONT_PREV | FLAG_CONT_NEXT | FLAG_ENCRYPTED)) {
        return false;
    }

    // ignore impossible or improbable header lengths
    uint16_t header_len = (uint16_t) int2(buf + OFFSET_HEAD_SIZE);
    if(header_len < FILE_HEAD_MIN_LEN || header_len > SUSPICIOUS_HEADER_LEN) {
        return false;
    }
    // abort if header is longer than the remaining buf
    if(header_len >= buf_len) {
        return false;
    }

    // ignore huge filename lengths
    uint16_t filename_bytes_len = (uint16_t) int2(buf + OFFSET_NAME_SIZE);
    if(filename_bytes_len > SUSPICIOUS_HEADER_LEN) {
        return false;
    }

    // ignore strange file sizes
    uint64_t& packed_size = output.compressed_size;
    uint64_t& unpacked_size = output.uncompressed_size;
    packed_size = (uint64_t) int4(buf + OFFSET_PACK_SIZE);
    unpacked_size = (uint64_t) int4(buf + OFFSET_UNP_SIZE);
    if(flags & FLAG_BIGFILE) {
        packed_size += ((uint64_t) int4(buf + OFFSET_HIGH_PACK_SIZE)) << 32;
        unpacked_size += ((uint64_t) int4(buf + OFFSET_HIGH_UNP_SIZE)) << 32;
    }
    // zero length, > 10 TiB, packed size significantly larger than
    // unpacked are all 'strange'
    if(packed_size == 0 || unpacked_size == 0 || packed_size * 0.95 > unpacked_size ||
            packed_size > SUSPICIOUS_FILE_LEN || unpacked_size > SUSPICIOUS_FILE_LEN)  {
        return false;
    }

    //
    // Filename extraction
    //
    string& filename = output.name;
    uint16_t filename_len = 0;
    const char *filename_bytes = (const char *) buf + OFFSET_FILE_NAME;
    if(flags & FLAG_BIGFILE) {
        // if present, the high 32 bits of 64 bit file sizes offset the
        // location of the filename by 8
        filename_bytes += OPTIONAL_BIGFILE_LEN;
    }
    if(flags & FLAG_UNICODE_FILENAME) {
        // The unicode filename flag can indicate two filename formats,
        // predicated on the presence of a null byte:
        //   - If a null byte is present, it separates an ASCII
        //     representation and a UTF-8 representation of the filename
        //     in that order
        //   - If no null byte is present, the filename is UTF-8 encoded
        size_t null_byte_index = 0;
        for(; null_byte_index < filename_bytes_len; null_byte_index++) {
            if(filename_bytes[null_byte_index] == 0x00) {
                break;
            }
        }

        if(null_byte_index == filename_bytes_len - 1u) {
            // Zero-length UTF-8 representation is illogical
            return false;
        }

        if(null_byte_index == filename_bytes_len) {
            // UTF-8 only
            filename_len = filename_bytes_len;
            filename = string(filename_bytes, (size_t) filename_len);
        }
        else {
            // if both ASCII and UTF-8 are present, disregard ASCII
            filename_len = filename_bytes_len - (null_byte_index + 1);
            filename = string(filename_bytes + null_byte_index + 1, filename_len);
        }
        // validate extracted UTF-8
        if(utf8::find_invalid(filename.begin(),filename.end()) != filename.end()) {
            return false;
        }
    }
    else {
        filename_len = filename_bytes_len;
        filename = string(filename_bytes, filename_len);
    }

    // throw out zero-length filename
    if(filename.size()==0) return false;

    // disallow ASCII control characters, which may also appear in valid UTF-8
    string::const_iterator first_control_character = filename.begin();
    for(; first_control_character != filename.end(); first_control_character++) {
        if((char) *first_control_character < ' ') {
            break;
        }
    }
    if(first_control_character != filename.end()) {
        return false;
    }


    // RAR version required to extract: do we want to abort if it's too new?
    output.unpack_version = buf[OFFSET_UNP_VER];
    output.compression_method = buf[OFFSET_METHOD];
    // OS that created archive
    output.host_os = buf[OFFSET_HOST_OS];
    // date (modification?) In DOS date format
    output.dos_time = int4(buf + OFFSET_FTIME);
    output.crc = int4(buf + OFFSET_FILE_CRC);
    output.file_attributes = int4(buf + OFFSET_ATTR);


    // header CRC is final validation; RAR stores only the 16 least
    // significant bytes of a CRC32
    uint16_t header_crc = int2(buf + OFFSET_HEAD_CRC);
    uint32_t calc_header_crc = crc_init();
    // Data accounted for in the CRC begins with the header type magic byte
    calc_header_crc = crc_update(calc_header_crc, buf + OFFSET_HEAD_TYPE, header_len - OFFSET_HEAD_TYPE);
    calc_header_crc = crc_finalize(calc_header_crc);
    bool head_crc_match = (header_crc == (calc_header_crc & 0xFFFF));
    if(!head_crc_match) {
        return false;
    }
    return true;
}

// volume processing (RAR file itself)
static bool process_volume(const unsigned char *buf, size_t buf_len, RarVolumeInfo &output)
{
    // confirm that the smallest possible component block header could fit in
    // the buffer
    if(buf_len < ARCHIVE_HEAD_MIN_LEN) {
        return false;
    }
    // Initial RAR file block anchor is 0x74 magic byte
    if(buf[OFFSET_HEAD_TYPE] != ARCHIVE_MAGIC) {
        return false;
    }
    // check for invalid flags
    uint16_t flags = (uint16_t) int2(buf + OFFSET_HEAD_FLAGS);
    if(flags & UNUSED_ARCHIVE_FLAGS) {
        return false;
    }

    // ignore impossible or improbable header lengths
    uint16_t header_len = (uint16_t) int2(buf + OFFSET_HEAD_SIZE);
    if(header_len < ARCHIVE_HEAD_MIN_LEN || header_len > SUSPICIOUS_HEADER_LEN) {
        return false;
    }
    // abort if header is longer than the remaining buf
    if(header_len >= buf_len) {
        return false;
    }

    // header CRC is final validation; RAR stores only the 16 least
    // significant bytes of a CRC32
    uint16_t header_crc = int2(buf + OFFSET_HEAD_CRC);
    uint32_t calc_header_crc = crc_init();
    // Data accounted for in the CRC begins with the header type magic byte
    calc_header_crc = crc_update(calc_header_crc, buf + OFFSET_HEAD_TYPE, header_len - OFFSET_HEAD_TYPE);
    calc_header_crc = crc_finalize(calc_header_crc);
    bool head_crc_match = (header_crc == (calc_header_crc & 0xFFFF));
    if(!head_crc_match) {
        return false;
    }

    return true;
}

static void unpack_buf(const uint8_t* input, size_t input_len, uint8_t* output, size_t output_len)
{
    // stupid unrar wants mutable strings for arg inputs
    char arg_bufs[6][32];
    strncpy(arg_bufs[0], "p", sizeof(arg_bufs[0]));
    strncpy(arg_bufs[1], "-y", sizeof(arg_bufs[1])); //say yes to everything
    strncpy(arg_bufs[2], "-ai", sizeof(arg_bufs[2])); //Ignore file attributes
    strncpy(arg_bufs[3], "-p-", sizeof(arg_bufs[3])); //Don't ask for password
    strncpy(arg_bufs[4], "-kb", sizeof(arg_bufs[4])); //Keep broken extracted files
    strncpy(arg_bufs[5], "aRarFile.rar", sizeof(arg_bufs[5])); //dummy file name
    char* args[6];
    args[0] = &arg_bufs[0][0];
    args[1] = &arg_bufs[1][0];
    args[2] = &arg_bufs[2][0];
    args[3] = &arg_bufs[3][0];
    //args[5] = "-inul"; //Disable all messages
    args[4] = &arg_bufs[4][0];
    args[5] = &arg_bufs[5][0];

    string xmloutput = "<rar>\n";
    CommandData data; //this variable is for assigning the commands to execute
    data.ParseCommandLine(6, args); //input the commands and have them parsed
    const wchar_t* c = L"aRarFile.rar"; //the 'L' prefix tells it to convert an ASCII Literal
    data.AddArcName("aRarFile.rar",c); //sets the name of the file

    CmdExtract extract; //from the extract.cpp file; allows the extraction to occur

    byte *startingaddress = (byte*) input;

    ComprDataIO mydataio;
    mydataio.SetSkipUnpCRC(true); //skip checking the CRC to allow more processing to occur
    mydataio.SetUnpackToMemory(output,output_len); //Sets flag to save output to memory

    extract.SetComprDataIO(mydataio); //Sets the ComprDataIO variable to the custom one that was just built

    extract.DoExtract(&data, startingaddress, input_len, xmloutput);

    data.Close();
}

// leave out depth checks for now
#if 0
/* See:
 * http://gcc.gnu.org/onlinedocs/gcc-4.5.0/gcc/Atomic-Builtins.html
 * for information on on __sync_fetch_and_add
 *
 * When rar_max_depth_count>=rar_max_depth_count_bypass,
 * hash the buffer before decompressing and do not decompress if it has already been decompressed.
 */

int scan_rar_name_len_max = 1024;
int rar_show_all=1;
uint32_t rar_max_depth_count = 0;
const uint32_t rar_max_depth_count_bypass = 5;
std::set<std::string>rar_seen_set;
cpp_mutex rar_seen_set_lock;
#endif
extern "C"
void scan_rar(const class scanner_params &sp,const recursion_control_block &rcb)
{
    assert(sp.sp_version==scanner_params::CURRENT_SP_VERSION);
    if(sp.phase==scanner_params::PHASE_STARTUP){
        assert(sp.info->si_version==scanner_info::CURRENT_SI_VERSION);
	sp.info->name  = "rar";
	sp.info->author = "Michael Shick";
	sp.info->description = "RAR volume locator and component decompresser";
	sp.info->feature_names.insert("rar");

        sp.info->get_config("rar_find_components",&record_components,"Search for RAR components");
        sp.info->get_config("raw_find_volumes",&record_volumes,"Search for RAR volumes");
	return;
    }
    if(sp.phase==scanner_params::PHASE_SCAN){
	const sbuf_t &sbuf = sp.sbuf;
	const pos0_t &pos0 = sp.sbuf.pos0;
	feature_recorder_set &fs = sp.fs;
	feature_recorder *rar_recorder = fs.get_name("rar");
	rar_recorder->set_flag(feature_recorder::FLAG_XML); // because we are sending through XML

        RarComponentInfo component;
        RarVolumeInfo volume;
	for(const unsigned char *cc=sbuf.buf; cc < sbuf.buf+sbuf.pagesize && cc < sbuf.buf + sbuf.bufsize; cc++) {
            size_t cc_len = sbuf.buf + sbuf.bufsize - cc;
            // feature files have three 'columns': forensic path / offset,
            // feature name, and feature data.  scan_zip is mimicked by having
            // the feature name be the compressed file's name (the component's
            // name) although this information is duplicated in the feature XML
            // data
            string feature_name = ".";
            string feature_data = "<null/>";
            ssize_t pos = cc-sbuf.buf; // position of the buffer

            // try each of the possible RAR blocks we may want to record
            if(record_volumes && process_volume(cc, cc_len, volume)) {
                rar_recorder->write(pos0 + pos, "<volume>", volume.to_xml());
            }
            if(record_components && process_component(cc, cc_len, component)) {
                rar_recorder->write(pos0 + pos, component.name, component.to_xml());
                //TODO decompression checks: size limit, recursion check
                managed_malloc<uint8_t>dbuf(component.uncompressed_size);
                if(component.compression_method != METHOD_UNCOMPRESSED) {
                    unpack_buf(cc, cc_len, dbuf.buf, component.uncompressed_size);

                    const pos0_t pos0_rar = pos0 + rcb.partName;
                    const sbuf_t child_sbuf(pos0_rar, dbuf.buf, component.uncompressed_size, sbuf.pagesize, false);
                    scanner_params child_params(sp, child_sbuf);

                    // call scanners on deobfuscated buffer
                    (*rcb.callback)(child_params);
                    if(rcb.returnAfterFound) {
                        return;
                    }
                }
            }
	}
    }
}
