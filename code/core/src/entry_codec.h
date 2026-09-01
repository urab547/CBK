// Copyright 2026 CBK Project. EntryMeta 与字节流的互转。
//
// 两个地方要用同一套编码：
//   · 容器的索引区（cbk list 只读索引就能列出全部条目，不用解数据区）
//   · cbk-native 打包器的条目头
// 抽出来共用，省得两边各写一份、然后慢慢长歪。
//
// 每条记录都带 U32 长度前缀，所以流式解析时可以"先读长度、再读整条、
// 最后在内存里解析"，不用为变长字段设计复杂的分段状态机。
#ifndef CODE_CORE_SRC_ENTRY_CODEC_H_
#define CODE_CORE_SRC_ENTRY_CODEC_H_

#include <cstdint>

#include "cbk/sink.h"
#include "cbk/types.h"
#include "src/byte_io.h"

namespace cbk {

/// 把一条 EntryMeta 编码成"U32 长度 + 记录体"写进 out。不含文件内容。
///
/// 路径类字段（relative_path / link_target）内存里是 UTF-16，
/// 这里转成 UTF-8 再写——容器里一律 UTF-8。
void WriteEntryMeta(const EntryMeta& meta, ISink& out);

/// 从已经装进内存的一条记录体里解析。reader 应当正好覆盖记录体，
/// 不含长度前缀。
///
/// @return 解析成功返回 true；字段不全或字符串越界返回 false。
bool DecodeEntryMeta(ByteReader* reader, EntryMeta* out);

/// 从流里读一条完整记录（长度前缀 + 记录体）并解析。
///
/// @return 流已经正常结束时返回 false 且 *end_of_stream 置 true；
///         记录损坏时返回 false 且 *end_of_stream 保持 false。
bool ReadEntryMeta(ISource& source, EntryMeta* out, bool* end_of_stream);

/// 单条记录的字节数上限。
///
/// 防的是"长度前缀被改坏成 40 亿，解析器直接去申请 4 GB 内存"这种情况。
/// 正常记录撑死几 KB（路径 32767 个 wchar 最多约 98 KB，SDDL 几百字节），
/// 1 MB 留了两个数量级的余量。
inline constexpr uint32_t kMaxEntryRecordSize = 1u << 20;

}  // namespace cbk

#endif  // CODE_CORE_SRC_ENTRY_CODEC_H_
