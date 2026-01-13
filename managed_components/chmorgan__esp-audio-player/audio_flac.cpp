#include <algorithm>
#include <cstring>
#include <cstdlib>

#include "audio_log.h"
#include "audio_flac.h"

static const char *TAG = "flac";

// Helpers to parse big-endian fields from STREAMINFO
// 辅助函数：FLAC 文件头部元数据是大端序 (Big-Endian)，这里将其转换为本机字节序
static uint16_t read_be16(const uint8_t *buf) {
    return (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
}

static uint32_t read_be24(const uint8_t *buf) {
    return (static_cast<uint32_t>(buf[0]) << 16) |
           (static_cast<uint32_t>(buf[1]) << 8) |
           buf[2];
}

static uint64_t read_be64(const uint8_t *buf) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | buf[i];
    }
    return value;
}

// 确保输出 PCM 缓冲区足够大。
// FLAC 的一帧可能包含 4096 个样本（甚至更多），这比 MP3 的 1152 要大，
// 所以动态调整内存非常重要，否则会溢出
static bool ensure_output_buffer(decode_data *output, size_t required_bytes) {
    // Allocate extra headroom so mono->stereo conversion (if needed elsewhere) still fits
    size_t alloc_bytes = required_bytes * 2;
    if ((alloc_bytes == 0) || (alloc_bytes / 2 != required_bytes)) {
        return false;
    }

    if (output->samples_capacity_max >= alloc_bytes) {
        output->samples_capacity = required_bytes;
        return true;
    }

    uint8_t *new_buf = static_cast<uint8_t*>(realloc(output->samples, alloc_bytes));
    if (!new_buf) {
        return false;
    }

    output->samples = new_buf;
    output->samples_capacity = required_bytes;
    output->samples_capacity_max = alloc_bytes;

    LOGI_2("grow pcm buffer to %d bytes", static_cast<int>(alloc_bytes));
    return true;
}

// ... flac_instance_init / free (内存生命周期管理) ...
void flac_instance_init(flac_instance *instance) {
    if (!instance) {
        return;
    }

    std::memset(&instance->ctx, 0, sizeof(instance->ctx));
    instance->data_buf = nullptr;
    instance->data_buf_size = 0;
    instance->bytes_in_data_buf = 0;
    instance->read_ptr = nullptr;
    instance->eof_reached = false;
    instance->data_start = 0;
}

void flac_instance_free(flac_instance *instance) {
    if (!instance) {
        return;
    }

    if (instance->ctx.decoded0) {
        free(instance->ctx.decoded0);
        instance->ctx.decoded0 = nullptr;
    }
    if (instance->ctx.decoded1) {
        free(instance->ctx.decoded1);
        instance->ctx.decoded1 = nullptr;
    }
    if (instance->data_buf) {
        free(instance->data_buf);
        instance->data_buf = nullptr;
    }

    instance->data_buf_size = 0;
    instance->bytes_in_data_buf = 0;
    instance->read_ptr = nullptr;
    instance->eof_reached = false;
    instance->data_start = 0;
}

// Parse STREAMINFO and size internal/output buffers to the advertised maxima
// FLAC 文件的开头必须是 "fLaC" 标记，紧接着是 STREAMINFO，元信息解析
// 这里包含了全局信息：最小/最大块大小、采样率、总样本数等。
static bool parse_stream_info(FILE *fp, flac_instance *instance, decode_data *output) {
    bool last_block = false;
    bool streaminfo_found = false;

    while (!last_block) {
        uint8_t header[4];
        if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
            return false;
        }
    // ... 读取 Header ...
    // FLAC 块头：第一位是 Last Block 标记，后7位是 Block Type (0 表示 STREAMINFO)
        last_block = (header[0] & 0x80) != 0;
        uint8_t block_type = header[0] & 0x7F;
        uint32_t block_length = (static_cast<uint32_t>(header[1]) << 16) |
                                (static_cast<uint32_t>(header[2]) << 8) |
                                header[3];
    // ... 解析 34 字节的 STREAMINFO ...
    // 关键：FLAC 允许每帧的 Blocksize 变化，解码器必须根据 Max Blocksize 分配内存
    // packed 变量包含了采样率(20bit), 通道(3bit), 位深(5bit), 总样本(36bit)
        if (block_type == 0 && block_length == 34) {
            uint8_t streaminfo[34];
            if (fread(streaminfo, 1, sizeof(streaminfo), fp) != sizeof(streaminfo)) {
                return false;
            }

            streaminfo_found = true;

            instance->ctx.min_blocksize = read_be16(streaminfo);
            instance->ctx.max_blocksize = read_be16(streaminfo + 2);
            instance->ctx.min_framesize = read_be24(streaminfo + 4);
            instance->ctx.max_framesize = read_be24(streaminfo + 7);

            uint64_t packed = read_be64(streaminfo + 10);
            instance->ctx.samplerate = static_cast<int>((packed >> 44) & 0xFFFFF);
            instance->ctx.channels = static_cast<int>(((packed >> 41) & 0x7) + 1);
            instance->ctx.bps = static_cast<int>(((packed >> 36) & 0x1F) + 1);
            instance->ctx.totalsamples = packed & 0xFFFFFFFFFULL;

    // 限制：这里只支持最多 2 声道。FLAC 标准支持 8 声道，但嵌入式 DAC 通常只有 2 个。
            if (instance->ctx.channels == 0 || instance->ctx.channels > 2) {
                return false;
            }
            if (instance->ctx.max_blocksize == 0) {
                return false;
            }

            size_t channels_out = (instance->ctx.channels == 1) ? 2 : instance->ctx.channels;
            size_t bytes_per_sample = (instance->ctx.bps > 16) ? sizeof(int32_t) : sizeof(int16_t);
            size_t required_bytes = instance->ctx.max_blocksize * channels_out * bytes_per_sample;

            if (!ensure_output_buffer(output, required_bytes)) {
                return false;
            }

            // Allocate internal decode buffers for FLACContext
            // ... 为解码核心分配内存 (ctx.decoded0/decoded1) ...
            // ... 初始化滑动读取缓冲区 data_buf ...
            if (instance->ctx.decoded0) {
                free(instance->ctx.decoded0);
                instance->ctx.decoded0 = nullptr;
            }
            if (instance->ctx.decoded1) {
                free(instance->ctx.decoded1);
                instance->ctx.decoded1 = nullptr;
            }

            instance->ctx.decoded0 = static_cast<int*>(malloc(sizeof(int) * instance->ctx.max_blocksize));
            if (!instance->ctx.decoded0) {
                return false;
            }
            if (instance->ctx.channels == 2) {
                instance->ctx.decoded1 = static_cast<int*>(malloc(sizeof(int) * instance->ctx.max_blocksize));
                if (!instance->ctx.decoded1) {
                    return false;
                }
            }

            size_t data_buffer_target = instance->ctx.max_framesize ?
                static_cast<size_t>(instance->ctx.max_framesize) + 16 :
                static_cast<size_t>(16 * 1024);
            data_buffer_target = std::max<size_t>(data_buffer_target, 4 * 1024);

            uint8_t *new_data_buf = static_cast<uint8_t*>(realloc(instance->data_buf, data_buffer_target));
            if (!new_data_buf) {
                return false;
            }

            instance->data_buf = new_data_buf;
            instance->data_buf_size = data_buffer_target;
            instance->read_ptr = instance->data_buf;
            instance->bytes_in_data_buf = 0;
            instance->eof_reached = false;
        } else {
            // Skip other metadata blocks，其他直接忽略
            if (fseek(fp, static_cast<long>(block_length), SEEK_CUR) != 0) {
                return false;
            }
        }
    }

    return streaminfo_found;
}
// // 打印 buffer 的辅助函数，用于调试看清文件头到底是什么
// static void print_hex_dump(const uint8_t *buf, size_t len) {
//     char hex_str[128] = {0};
//     int pos = 0;
//     for (size_t i = 0; i < len && i < 32; i++) { // 只打印前32字节
//         pos += sprintf(hex_str + pos, "%02X ", buf[i]);
//     }
//     ESP_LOGI(TAG,"File Header Hex: %s", hex_str);
//     char char_str[128] = {0};
//     pos = 0;
//     for (size_t i = 0; i < len && i < 32; i++) {
//         char_str[pos++] = (buf[i] >= 32 && buf[i] <= 126) ? buf[i] : '.';
//     }
//     char_str[pos] = 0;
//     ESP_LOGI(TAG,"File Header ASCII: %s", char_str);
// }

// bool is_flac(FILE *fp, decode_data *output, flac_instance *instance) {
//     if (!fp || !instance || !output) {
//         return false;
//     }

//     fseek(fp, 0, SEEK_SET);

//     // 1. 读取文件头部的这一大块数据 (例如 4KB)，用于搜索
//     // 很多 ID3v2 标签包含高清封面，可能很大，所以搜索范围要适度
//     #define SCAN_SIZE 4096 
//     uint8_t *scan_buf = (uint8_t *)malloc(SCAN_SIZE);
//     if (!scan_buf) {
//         return false;
//     }

//     size_t read_len = fread(scan_buf, 1, SCAN_SIZE, fp);
//     if (read_len < 4) {
//         free(scan_buf);
//         return false;
//     }

//     // 调试：打印文件头，看看究竟是什么牛鬼蛇神
//     print_hex_dump(scan_buf, read_len);

//     // 2. 暴力搜索 "fLaC" 标志
//     long flac_offset = -1;
    
//     // 检查是否是 Ogg FLAC (以 "OggS" 开头)
//     if (read_len >= 4 && memcmp(scan_buf, "OggS", 4) == 0) {
//         ESP_LOGI(TAG,"Error: This is an Ogg FLAC file. The current decoder only supports Native FLAC.");
//         free(scan_buf);
//         return false;
//     }

//     for (size_t i = 0; i <= read_len - 4; i++) {
//         if (scan_buf[i] == 'f' && 
//             scan_buf[i+1] == 'L' && 
//             scan_buf[i+2] == 'a' && 
//             scan_buf[i+3] == 'C') {
//             flac_offset = (long)i;
//             break;
//         }
//     }

//     free(scan_buf); // 释放搜索缓冲

//     if (flac_offset < 0) {
//         ESP_LOGI(TAG,"Magic 'fLaC' not found in the first %d bytes", SCAN_SIZE);
//         fseek(fp, 0, SEEK_SET);
//         return false;
//     }

//     ESP_LOGI(TAG,"Found 'fLaC' magic at offset: %ld", flac_offset);

//     // 3. 定位到 fLaC 标志之后
//     // fseek 到 (offset + 4)，跳过 "fLaC" 这4个字节，直接指向 StreamInfo Block
//     if (fseek(fp, flac_offset + 4, SEEK_SET) != 0) {
//         return false;
//     }

//     // 重置运行时状态
//     flac_instance_init(instance); // 建议使用复位函数，或者手动重置如下：
//     instance->bytes_in_data_buf = 0;
//     instance->read_ptr = instance->data_buf;
//     instance->eof_reached = false;
//     instance->ctx.sample_skip = 0;
//     instance->ctx.framesize = 0;
//     instance->ctx.samplenumber = 0;
//     instance->ctx.bitstream_size = 0;
//     instance->ctx.bitstream_index = 0;

//     // 4. 解析 StreamInfo
//     // parse_stream_info 期望 fp 指向 Metadata Block Header (就在 fLaC 后面)
//     bool ok = parse_stream_info(fp, instance, output);
//     if (!ok) {
//         LOGI_1("Failed to parse StreamInfo after finding magic");
//         fseek(fp, 0, SEEK_SET);
//         return false;
//     }

//     // 5. 记录数据开始位置
//     instance->data_start = ftell(fp);
//     fseek(fp, instance->data_start, SEEK_SET);

//     ESP_LOGI(TAG,"flac: sr=%d ch=%d bps=%d max_block=%d start=%ld", 
//            instance->ctx.samplerate,
//            instance->ctx.channels, instance->ctx.bps, 
//            instance->ctx.max_blocksize, instance->data_start);
//     return true;
// }

// 辅助函数：解析 ID3v2 的 Syncsafe 整数（每个字节只用低7位）
static uint32_t read_id3_size(const uint8_t *ptr) {
    return ((ptr[0] & 0x7F) << 21) |
           ((ptr[1] & 0x7F) << 14) |
           ((ptr[2] & 0x7F) << 7)  |
            (ptr[3] & 0x7F);
}


// 判断是否为 FLAC 文件并初始化
//D3v2 标签头为 10 个字节，结构如下：
// ID3 (3 bytes)
// Version (2 bytes)
// Flags (1 byte)
// Size (4 bytes, Syncsafe integer)
bool is_flac(FILE *fp, decode_data *output, flac_instance *instance) {
    if (!fp || !instance || !output) {
        return false;
    }

    fseek(fp, 0, SEEK_SET);

    uint8_t header[10];
    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        return false;
    }

    // 1. 检查是否有 ID3v2 标签头
    if (std::memcmp(header, "ID3", 3) == 0) {
        // 计算 ID3 标签内容的大小
        uint32_t tag_size = read_id3_size(header + 6);
        // ID3 头部本身占 10 字节，所以总偏移量 = 10 + 内容大小
        size_t skip_bytes = 10 + tag_size;
        
        ESP_LOGI(TAG, "Found ID3v2 tag, skipping %d bytes", (int)skip_bytes);
        
        // 跳过 ID3 标签
        if (fseek(fp, skip_bytes, SEEK_SET) != 0) {
            return false;
        }
        
        // 重新读取接下来的 4 个字节，检查是否为 fLaC
        if (fread(header, 1, 4, fp) != 4) {
            return false;
        }
    } else {
        // 不是 ID3，回退到文件开头，准备检查 fLaC
        fseek(fp, 0, SEEK_SET);
        if (fread(header, 1, 4, fp) != 4) {
            return false;
        }
    }

    uint8_t magic[4];
    if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic)) {
        return false;
    }

    if (std::memcmp(magic, "fLaC", sizeof(magic)) != 0) {
        fseek(fp, 0, SEEK_SET);
        return false;
    }

    // Reset runtime state but keep already allocated buffers if present
    instance->bytes_in_data_buf = 0;
    instance->read_ptr = instance->data_buf;
    instance->eof_reached = false;
    instance->ctx.sample_skip = 0;
    instance->ctx.framesize = 0;
    instance->ctx.samplenumber = 0;
    instance->ctx.bitstream_size = 0;
    instance->ctx.bitstream_index = 0;
    // 解析 StreamInfo
    bool ok = parse_stream_info(fp, instance, output);
    if (!ok) {
        fseek(fp, 0, SEEK_SET);
        return false;
    }
    //ftell用于获取文件指针在文件中的当前位置。
    instance->data_start = ftell(fp);
    //将文件开始位置向后移动，跳过文件头和元数据块，定位到音频数据的起始位置
    fseek(fp, instance->data_start, SEEK_SET);

    ESP_LOGI(TAG, "flac: sr=%d ch=%d bps=%d max_block=%d", instance->ctx.samplerate,
           instance->ctx.channels, instance->ctx.bps, instance->ctx.max_blocksize);
    return true;
}

// FLAC音乐会有多个音乐帧，每个帧的长度不固定，且都有自己的帧头
// Refill the sliding input buffer while preserving unread bytes
// 核心 I/O 逻辑：滑动窗口缓冲区填充
// FLAC 帧长度不固定。如果缓冲区剩下的数据不够一帧，需要把剩下的数据搬到头部，
// 然后从文件读取新数据填满缓冲区。
// 
static DECODE_STATUS handle_refill(FILE *fp, flac_instance *inst) {
    //未读取
    size_t unread = inst->bytes_in_data_buf - static_cast<size_t>(inst->read_ptr - inst->data_buf);
    bool need_refill = (unread < (inst->data_buf_size / 2)) && !inst->eof_reached;
    // memmove 处理未读数据 -> fread 填充 -> 更新指针
    if (need_refill) {
        //先把没有读的数据搬到缓冲区头部
        memmove(inst->data_buf, inst->read_ptr, unread);

        size_t free_space = inst->data_buf_size - unread;
        //然后从文件读取新数据填充缓冲区剩余空间，不一定与空余空间相同
        size_t n_read = fread(inst->data_buf + unread, 1, free_space, fp);

        inst->bytes_in_data_buf = unread + n_read;
        //重置读指针到缓冲区头部
        inst->read_ptr = inst->data_buf;
        //小于看看是否读到文件尾了
        if (n_read < free_space) {
            inst->eof_reached = feof(fp);
        }

        ESP_LOGI(TAG, "refill: pos %ld unread %d n_read %d eof %d", ftell(fp), (int)unread, (int)n_read, inst->eof_reached);
    }

    if (inst->bytes_in_data_buf == 0) {
        return inst->eof_reached ? DECODE_STATUS_DONE : DECODE_STATUS_NO_DATA_CONTINUE;
    }

    return DECODE_STATUS_CONTINUE;
}

// 主解码循环：被上层音频任务循环调用
DECODE_STATUS decode_flac(FILE *fp, decode_data *pData, flac_instance *pInstance) {
    if (!fp || !pData || !pInstance) {
        return DECODE_STATUS_ERROR;
    }
    //先填充缓冲区
    DECODE_STATUS refill_status = handle_refill(fp, pInstance);
    if (refill_status != DECODE_STATUS_CONTINUE) {
        return refill_status;
    }
    //填充后解码前定位帧头
    size_t unread = pInstance->bytes_in_data_buf - static_cast<size_t>(pInstance->read_ptr - pInstance->data_buf);
    // Locate next frame sync inside the sliding buffer
    int offset = flac_seek_frame(pInstance->read_ptr, static_cast<uint32_t>(unread), &pInstance->ctx);
    LOGI_2("seek: unread %d offset %d", (int)unread, offset);
    
    //找不到帧头
    if (offset < 0) {
        if (pInstance->eof_reached) {
            return DECODE_STATUS_DONE;
        }
        // Drop buffer and try to refill next call
        pInstance->read_ptr = pInstance->data_buf;
        pInstance->bytes_in_data_buf = 0;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }

    //frame_buf_size除去帧头偏移，剩下的buffer中的大小（输入数组），准备解码
    uint8_t *frame_ptr = pInstance->read_ptr + offset;
    int frame_buf_size = static_cast<int>(unread - static_cast<size_t>(offset));

    int decode_result = 0;
    size_t channels_out = (pInstance->ctx.channels == 1) ? 2 : pInstance->ctx.channels;
    size_t bytes_per_sample = (pInstance->ctx.bps > 16) ? sizeof(int32_t) : sizeof(int16_t);
    size_t required_bytes = static_cast<size_t>(pInstance->ctx.max_blocksize) * channels_out * bytes_per_sample;

    if (required_bytes > pData->samples_capacity_max) {
        ESP_LOGE(TAG, "flac frame requires %d bytes, buffer %d", static_cast<int>(required_bytes),
                 static_cast<int>(pData->samples_capacity_max));
        return DECODE_STATUS_ERROR;
    }
    //在此处解码的时候会更新frame_size
    
    if (pInstance->ctx.bps > 16) {
        // int32_t *s = reinterpret_cast<int32_t*>(pData->samples);
        // //I2S 配置是否 32bit 槽+MSB 对齐：clk_set_fn/驱动应与数据对齐方式一致；
        // size_t samples = pInstance->ctx.blocksize * channels_out;
        // for (size_t k = 0; k < samples; ++k) s[k] <<= 8; // 24bit -> 32bit MSB对齐
        decode_result = flac_decode_frame24(&pInstance->ctx, frame_ptr, frame_buf_size,
                                            reinterpret_cast<int32_t*>(pData->samples));
        pData->fmt.bits_per_sample = 32;
    } else {
        decode_result = flac_decode_frame16(&pInstance->ctx, frame_ptr, frame_buf_size,
                                            reinterpret_cast<int16_t*>(pData->samples));
        pData->fmt.bits_per_sample = 16;
    }

    if (decode_result != 0) {
        ESP_LOGE(TAG, "flac decode error %d", decode_result);
        // Attempt to skip past this frame to continue playback

        size_t consumed = std::max<size_t>(1, pInstance->ctx.framesize + offset);
        //更新读指针
        pInstance->read_ptr += consumed;
        if (pInstance->read_ptr > pInstance->data_buf + pInstance->bytes_in_data_buf) {
            pInstance->read_ptr = pInstance->data_buf;
            pInstance->bytes_in_data_buf = 0;
        }
        return pInstance->eof_reached ? DECODE_STATUS_DONE : DECODE_STATUS_NO_DATA_CONTINUE;
    }
        //消耗总字节数
    size_t consumed_bytes = static_cast<size_t>(pInstance->ctx.framesize) + static_cast<size_t>(offset);
    if (consumed_bytes > unread) {
        consumed_bytes = unread;
    }
    //解读当前帧后，更新滑动缓冲区大小
    size_t remaining = unread - consumed_bytes;
    uint8_t *next_start = frame_ptr + pInstance->ctx.framesize;
    // Compact unread data to buffer front so the next seek starts at position 0
    memmove(pInstance->data_buf, next_start, remaining);
    //更新指针
    pInstance->read_ptr = pInstance->data_buf;
    pInstance->bytes_in_data_buf = remaining;
        LOGI_2("ok: sr %d ch %d bps %d fc %d framesize %d remaining %d", pInstance->ctx.samplerate,
            pInstance->ctx.channels, pInstance->ctx.bps, (int)pInstance->ctx.blocksize, (int)pInstance->ctx.framesize,
            (int)remaining);
    //输出更新（供 I2S）：
    pData->fmt.sample_rate = pInstance->ctx.samplerate;
    pData->fmt.channels = static_cast<uint32_t>(channels_out);
    pData->frame_count = static_cast<size_t>(pInstance->ctx.blocksize);

    return DECODE_STATUS_CONTINUE;
}
