#include "heapfile/HeapFile.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

// ============================================================================
// HeapFile — STUDENT IMPLEMENTATION FILE
// ============================================================================
//
// Every member method declared in heapfile/HeapFile.h has a blank body
// below; the header documents the contract of each one (the file-level
// comment there explains the page format, the two free-space lists, and
// the pin protocol). Your task: replace the stub bodies with working
// implementations until the check suite (`p2stdbeng`) passes.
//
// What is already provided (do not change): the file-local helpers in the
// anonymous namespace below — the header-page layout constants, the
// HeaderFields struct and its read/write helpers, and the identification
// record writers/checkers. They fix the on-disk format; your member
// methods build on them. The free function erase_file at the bottom is
// also complete.

namespace heapfile {

namespace {

// Releases a pin without disturbing `error`, which on failure paths already
// carries the real cause (unpin clears the string it is given).
void release(bufman::BufferManager& manager, std::size_t frame, bool was_dirty) {
    std::string unpin_error;
    manager.unpin(frame, was_dirty, unpin_error);
}

// The header page's record area (its slots are never used): magic, the five
// structural fields, then the table name. Fixed-width fields come before
// the variable-length name so every field has a constant offset.
//
//   0   magic "TUP2"
//   4   free_head     first page with a free slot (kNoPage when empty)
//   8   free_tail     last page appended to the free list
//   12  full_head     first page whose every slot is used
//   16  full_tail
//   20  next_page_id  the id the next page allocation will use (starts at 1)
//   24  name_length
//   28  name bytes
struct HeaderFields {
    std::uint32_t free_head = kNoPage;
    std::uint32_t free_tail = kNoPage;
    std::uint32_t full_head = kNoPage;
    std::uint32_t full_tail = kNoPage;
    std::uint32_t next_page_id = 1;
};

constexpr std::uint32_t kHeapMagic = 0x32505554u; // "TUP2"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kHeaderFreeHeadOffset = 4;
constexpr std::size_t kHeaderFreeTailOffset = 8;
constexpr std::size_t kHeaderFullHeadOffset = 12;
constexpr std::size_t kHeaderFullTailOffset = 16;
constexpr std::size_t kHeaderNextPageIdOffset = 20;
constexpr std::size_t kNameLengthOffset = 24;
constexpr std::size_t kNameOffset = 28;
constexpr std::size_t kMaxTableNameLength = 200;

std::size_t head_offset(bool free_list) {
    return free_list ? kHeaderFreeHeadOffset : kHeaderFullHeadOffset;
}

std::size_t tail_offset(bool free_list) {
    return free_list ? kHeaderFreeTailOffset : kHeaderFullTailOffset;
}

HeaderFields read_header_fields(const char* buffer) {
    HeaderFields fields;
    fields.free_head = page_detail::read_u32(buffer, kHeaderFreeHeadOffset);
    fields.free_tail = page_detail::read_u32(buffer, kHeaderFreeTailOffset);
    fields.full_head = page_detail::read_u32(buffer, kHeaderFullHeadOffset);
    fields.full_tail = page_detail::read_u32(buffer, kHeaderFullTailOffset);
    fields.next_page_id = page_detail::read_u32(buffer, kHeaderNextPageIdOffset);
    return fields;
}

void write_header_fields(char* buffer, const HeaderFields& fields) {
    page_detail::write_u32(buffer, kHeaderFreeHeadOffset, fields.free_head);
    page_detail::write_u32(buffer, kHeaderFreeTailOffset, fields.free_tail);
    page_detail::write_u32(buffer, kHeaderFullHeadOffset, fields.full_head);
    page_detail::write_u32(buffer, kHeaderFullTailOffset, fields.full_tail);
    page_detail::write_u32(buffer, kHeaderNextPageIdOffset, fields.next_page_id);
}

// Writes the identification record into a bound header-page buffer: magic,
// empty lists, allocation frontier at 1, table name. Returns false with a
// named error when the table name is too long.
bool write_identification(char* buffer, const schema::TableSchema& schema,
                          std::string& error) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        error = "table name is " + std::to_string(name.size()) +
                " bytes; at most " + std::to_string(kMaxTableNameLength) +
                " fit the header page";
        return false;
    }
    page_detail::write_u32(buffer, kMagicOffset, kHeapMagic);
    write_header_fields(buffer, HeaderFields{});
    page_detail::write_u32(buffer, kNameLengthOffset,
                           static_cast<std::uint32_t>(name.size()));
    std::memcpy(buffer + kNameOffset, name.data(), name.size());
    return true;
}

// Checks the identification record against `schema`. False with a named
// error when the magic is missing or the name differs.
bool check_identification(const char* buffer,
                          const schema::TableSchema& schema,
                          std::string& error) {
    if (page_detail::read_u32(buffer, kMagicOffset) != kHeapMagic) {
        error = "not a tuple heap file";
        return false;
    }
    const std::uint32_t length = page_detail::read_u32(buffer, kNameLengthOffset);
    if (length > kMaxTableNameLength) {
        error = "not a tuple heap file";
        return false;
    }
    const std::string stored(buffer + kNameOffset, length);
    if (stored != schema.table_name()) {
        error = "heap file is for table '" + stored + "'";
        return false;
    }
    return true;
}

// The list pointers must name allocated pages (or be empty). A dirty page
// may not be on disk yet, so next_page_id can legitimately exceed the
// on-disk block count by at most the pool's frame count; anything beyond
// that is a corrupt header, not slack.
bool header_fields_in_range(const HeaderFields& fields, std::uint64_t blocks,
                            std::string& error) {
    constexpr std::uint64_t kDirtySlack = 4096; // >= any pool this engine builds
    const bool ok =
        fields.next_page_id >= 1 &&
        fields.next_page_id <= blocks + kDirtySlack &&
        (fields.free_head == kNoPage || fields.free_head < fields.next_page_id) &&
        (fields.free_tail == kNoPage || fields.free_tail < fields.next_page_id) &&
        (fields.full_head == kNoPage || fields.full_head < fields.next_page_id) &&
        (fields.full_tail == kNoPage || fields.full_tail < fields.next_page_id);
    if (!ok) {
        error = "header list pointers are out of range";
    }
    return ok;
}

}

// ---------------------------------------------------------------------------
// Member methods — YOUR WORK STARTS HERE
// ---------------------------------------------------------------------------

HeapFile::HeapFile(bufman::BufferManager& manager) : manager_(manager) {}

bool HeapFile::is_open() const {
    // TODO(student): report whether a file is currently open.
    return open_;
}

bool HeapFile::require_open(std::string& error) const {
    // TODO(student): fail with "heap file is not open" when closed.
    if (!open_) {
        error = "heap file is not open";
        return false;
    }
    return true;
}

bool HeapFile::create(const std::string& path,
                      const schema::TableSchema& schema, std::string& error) {
    // TODO(student): format a one-block file: refuse an open HeapFile / a
    // non-empty path, validate the record size, alloc page 0, format it as
    // a SlottedPage, write the identification record, and close (flushing)
    // the file — leaving this HeapFile closed.
    error.clear();
    if (open_) {
        error = "cannot create \"" + path + "\": this heap file is already open";
        return false;
    }
    const schema::TupleSerializer serializer(schema);
    const std::size_t size = serializer.record_size();
    if (size == 0) {
        error = "cannot create \"" + path + "\": the catalog has no record layout (no columns, or "
        "String columns without declared width)";
        return false;
    }
    if (size > bufman::kPageSize - kPageHeaderSize - 1) {
        error = "cannot create \"" + path + "\": a " + std::to_string(bufman::kPageSize) +
        "-byte page fits records of at most " + std::to_string(bufman::kPageSize - kPageHeaderSize - 1) +
        " bytes, the catalog needs " + std::to_string(size);
        return false;
    }
    bufman::File file;
    if (!manager_.open_file(path, file, error)) {
        return false;
    }
    const std::uint64_t blocks = bufman::block_count(file, error);
    if (!error.empty()) {
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    if (blocks > 0) {
        error = "cannot create \"" + path + "\": the file already holds " + std::to_string(blocks) + " block(s)";
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    std::size_t frame = 0;
    if (!manager_.alloc_page(file, 0, frame, error)) {
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    SlottedPage header;
    header.bind(manager_.frame(frame).buffer, size);
    header.format();
    if (!write_identification(manager_.frame(frame).buffer, schema, error)) {
        release(manager_, frame, false);
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    release(manager_, frame, true);
    manager_.close_file(file, error);
    return error.empty();
}

bool HeapFile::open(const std::string& path,
                    const schema::TableSchema& schema, std::string& error) {
    // TODO(student): probe that the path exists, register it with the
    // manager, then validate block 0: identification, SlottedPage attach,
    // and the header fields in range. On any failure unwind completely
    // (close through the manager, reset the handle) and return false; on
    // success bind schema_/serializer_/record_size_ and mark the file open.
    error.clear();
    if (open_) {
        error = "cannot open \"" + path + "\": this heap file is already open";
        return false;
    }
    const schema::TupleSerializer serializer(schema);
    const std::size_t size = serializer.record_size();
    if (size == 0) {
        error = "cannot open \"" + path + "\": the catalog has no record layout (no columns, or "
        "String columns without declared width)";
        return false;
    }
    if (size > bufman::kPageSize - kPageHeaderSize - 1) {
        error = "cannot open \"" + path + "\": a " + std::to_string(bufman::kPageSize) +
        "-byte page fits records of at most " + std::to_string(bufman::kPageSize - kPageHeaderSize - 1) +
        " bytes, the catalog needs " + std::to_string(size);
        return false;
    }
    bufman::File probe;
    if (!bufman::open_for_read(path, probe, error)) {
        error = "cannot open \"" + path + "\": " + error;
        return false;
    }
    bufman::close(probe);

    if (!manager_.open_file(path, file_, error)) {
        return false;
    }
    const std::uint64_t blocks = bufman::block_count(file_, error);
    if (error.empty() && blocks == 0) {
        error = "cannot open \"" + path + "\": the file has no header page";
    }
    std::size_t frame = 0;
    if (error.empty() && manager_.pin(file_, 0, frame, error)) {
        SlottedPage header;
        header.bind(manager_.frame(frame).buffer, size);
        if (!check_identification(manager_.frame(frame).buffer, schema, error)) {
            error = "cannot open \"" + path + "\": " + error;
            } else if (!header.attach(error)) {
                error = "cannot open \"" + path + "\": " + error;
            } else {
                const HeaderFields fields = read_header_fields(manager_.frame(frame).buffer);

                if (!header_fields_in_range(fields, blocks, error)) {
                    error = "cannot open \"" + path + "\": " + error;
                }
            }
        release(manager_, frame, false);
    }
    if (!error.empty()) {
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        return false;
    }
    schema_ = &schema;
    serializer_.emplace(schema);
    record_size_ = size;
    open_ = true;
    return true;
}

HeapFileIterator HeapFile::scan() {
    // TODO(student): pin the header once, read the two list heads and
    // next_page_id, unpin, and return an iterator seeded with them. A
    // closed file yields an exhausted iterator.
    if (!is_open()) {
        return HeapFileIterator(*this, kNoPage, kNoPage, 1);
    }
    std::size_t frame = 0;
    std::string error;

    if (!manager_.pin(file_, 0, frame, error)) {
        return HeapFileIterator(*this, kNoPage, kNoPage, 1);
    }
    const HeaderFields header = read_header_fields(manager_.frame(frame).buffer);
    release(manager_, frame, false);
    return HeapFileIterator(*this, header.free_head, header.full_head, header.next_page_id);
}

// --- List surgery -----------------------------------------------------------

bool HeapFile::append_fresh_page(std::uint32_t page_id, std::size_t page_frame,
                                 std::string& error) {
    // TODO(student): make the just-allocated page the free list's only
    // element (head and tail) and advance next_page_id past it; clear the
    // page's own links. Only called right after an allocation, when the
    // free list is empty by construction — refuse otherwise.
    error.clear();
    if (!require_open(error)) {
        return false;
    }

    std::size_t header_frame = 0;
    if (!manager_.pin(file_, 0, header_frame, error)) {
        return false;
    }
    HeaderFields fields =
        read_header_fields(manager_.frame(header_frame).buffer);

    if (fields.free_head != kNoPage || fields.free_tail != kNoPage) {
        error = "cannot append fresh page: free list is not empty";
        release(manager_, header_frame, false);
        return false;
    }
    if (page_id == kNoPage || page_id != fields.next_page_id) {
        error = "cannot append fresh page: unexpected page id";
        release(manager_, header_frame, false);
        return false;
    }

    SlottedPage page;
    page.bind(manager_.frame(page_frame).buffer, record_size_);
    if (!page.attach(error)) {
        release(manager_, header_frame, false);
        return false;
    }

    page.set_prev_page(kNoPage);
    page.set_next_page(kNoPage);
    fields.free_head = page_id;
    fields.free_tail = page_id;
    ++fields.next_page_id;
    write_header_fields(manager_.frame(header_frame).buffer, fields);
    release(manager_, header_frame, true);
    return true;
}

bool HeapFile::move_between_lists(bool to_full, std::uint32_t page_id,
                                  std::size_t page_frame, std::string& error) {
    // TODO(student): detach the pinned page from its source list (its
    // prev/next name the source neighbors) and append it at the
    // destination list's tail. Acquire ALL auxiliary pins first (header +
    // source neighbors + destination tail) in ascending page-id order;
    // after that the surgery is pure memory writes.
    error.clear();
    if (!require_open(error)) {
        return false;
    }
    SlottedPage moving;
    moving.bind(manager_.frame(page_frame).buffer, record_size_);
    if (!moving.attach(error)) {
        return false;
    }
    const std::uint32_t old_prev = moving.prev_page();
    const std::uint32_t old_next = moving.next_page();
    std::size_t header_frame = 0;
    if (!manager_.pin(file_, 0, header_frame, error)) {
        return false;
    }
    HeaderFields fields =
        read_header_fields(manager_.frame(header_frame).buffer);
    std::uint32_t& source_head =
        to_full ? fields.free_head : fields.full_head;
    std::uint32_t& source_tail =
        to_full ? fields.free_tail : fields.full_tail;
    std::uint32_t& target_head =
        to_full ? fields.full_head : fields.free_head;
    std::uint32_t& target_tail =
        to_full ? fields.full_tail : fields.free_tail;

    const std::uint32_t old_target_tail = target_tail;
    if (page_id == 0 || page_id >= fields.next_page_id || source_head == kNoPage || source_tail == kNoPage ||
        (target_head == kNoPage) != (target_tail == kNoPage) ||
        (old_prev == kNoPage) != (source_head == page_id) ||
        (old_next == kNoPage) != (source_tail == page_id)) {
        error = "inconsistent heap-file list pointers";
        release(manager_, header_frame, false);
        return false;
    }

    std::vector<std::uint32_t> ids;
    for (std::uint32_t id : {old_prev, old_next, old_target_tail}) {
        if (id == kNoPage) {
            continue;
        }
        if (id == 0 || id == page_id ||
            id >= fields.next_page_id) {
            error = "inconsistent heap-file list neighbor";
            release(manager_, header_frame, false);
            return false;
        }
        ids.push_back(id);
    }

    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        error = "a page appears twice among list neighbors";
        release(manager_, header_frame, false);
        return false;
    }

    struct Auxiliary {
        std::uint32_t id;
        std::size_t frame;
        SlottedPage page;
    };
    std::vector<Auxiliary> auxiliary;
    auxiliary.reserve(ids.size());
    auto release_auxiliary = [&](bool dirty) {
        for (auto& entry : auxiliary) {
            release(manager_, entry.frame, dirty);
        }
    };
    for (std::uint32_t id : ids) {
        std::size_t frame = 0;
        if (!manager_.pin(file_, id, frame, error)) {
            release_auxiliary(false);
            release(manager_, header_frame, false);
            return false;
        }
        SlottedPage page;
        page.bind(manager_.frame(frame).buffer, record_size_);
        if (!page.attach(error)) {
            release(manager_, frame, false);
            release_auxiliary(false);
            release(manager_, header_frame, false);
            return false;
        }
        auxiliary.push_back({id, frame, page});
    }

    auto page_for = [&](std::uint32_t id) -> SlottedPage* {
        for (auto& entry : auxiliary) {
            if (entry.id == id) {
                return &entry.page;
            }
        }
        return nullptr;
    };

    if ((old_prev != kNoPage && page_for(old_prev)->next_page() != page_id) ||
        (old_next != kNoPage && page_for(old_next)->prev_page() != page_id) ||
        (old_target_tail != kNoPage && page_for(old_target_tail)->next_page() != kNoPage)) {
        error = "inconsistent heap-file neighbor links";
        release_auxiliary(false);
        release(manager_, header_frame, false);
        return false;
    }

    if (old_prev != kNoPage) {
        page_for(old_prev)->set_next_page(old_next);
    } else {
        source_head = old_next;
    }
    if (old_next != kNoPage) {
        page_for(old_next)->set_prev_page(old_prev);
    } else {
        source_tail = old_prev;
    }

    moving.set_prev_page(old_target_tail);
    moving.set_next_page(kNoPage);
    if (old_target_tail != kNoPage) {
        page_for(old_target_tail)->set_next_page(page_id);
    } else {
        target_head = page_id;
    }
    target_tail = page_id;

    write_header_fields(manager_.frame(header_frame).buffer, fields);
    release_auxiliary(true);
    release(manager_, header_frame, true);
    return true;
}

// --- Row operations ---------------------------------------------------------

bool HeapFile::insert(const tuple::Tuple& tuple, RowId& row_id,
                      std::string& error) {
    // TODO(student): serialize (validates first), take the free list's
    // head as the target page — or allocate a fresh page and append it to
    // the free list when the free list is empty — store the record in the
    // target's first free slot, and move the page to the full list when
    // its last free slot was taken. Report the new row id.
    error.clear();
    if (!require_open(error)) {
        return false;
    }
    std::array<char, bufman::kPageSize> record{};
    if (!serializer_->serialize(tuple, record.data(), record.size(), error)) {
        return false;
    }
    std::size_t header_frame = 0;
    if (!manager_.pin(file_, 0, header_frame, error)) {
        return false;
    }
    const HeaderFields fields = read_header_fields(manager_.frame(header_frame).buffer);
    std::uint32_t page_id = fields.free_head;
    std::size_t frame = 0;
    SlottedPage page;
    std::optional<std::size_t> reserved_tail;
    if (page_id == kNoPage) {
        if (slot_capacity(record_size_) == 1 && fields.full_tail != kNoPage) {
            std::size_t tail_frame = 0;
            if (!manager_.pin(file_, fields.full_tail, tail_frame, error)) {
                release(manager_, header_frame, false);
                return false;
            }
            reserved_tail = tail_frame;
        }
        page_id = fields.next_page_id;
        if (!manager_.alloc_page(file_, page_id, frame, error)) {
            if (reserved_tail) {
                release(manager_, *reserved_tail, false);
            }
            release(manager_, header_frame, false);
            return false;
        }
        page.bind(manager_.frame(frame).buffer, record_size_);
        page.format();
        if (!append_fresh_page(page_id, frame, error)) {
            release(manager_, frame, true);
            if (reserved_tail) {
                release(manager_, *reserved_tail, false);
            }
            release(manager_, header_frame, false);
            return false;
        }
        release(manager_, header_frame, false);
    } else {
        release(manager_, header_frame, false);
        if (!manager_.pin(file_, page_id, frame, error)) {
            return false;
        }
        page.bind(manager_.frame(frame).buffer, record_size_);
        if (!page.attach(error)) {
            release(manager_, frame, false);
            return false;
        }
    }
    const std::optional<std::size_t> free_slot = page.first_free_slot();
    if (!free_slot) {
        error = "FREE list page " + std::to_string(page_id) + " has no free record slot";
        release(manager_, frame, false);
        if (reserved_tail) {
            release(manager_, *reserved_tail, false);
        }
        return false;
    }
    std::vector<char> previous_bytes(record_size_);
    char* slot_bytes = manager_.frame(frame).buffer + *free_slot * record_size_;
    std::memcpy(previous_bytes.data(), slot_bytes, record_size_);
    if (!page.put_record(*free_slot, record.data(), record_size_, error)) {
        release(manager_, frame, false);
        if (reserved_tail) {
            release(manager_, *reserved_tail, false);
        }
        return false;
    }
    if (page.occupied_count() == page.capacity()) {
        if (!move_between_lists(true, page_id, frame, error)) {
            const std::string transition_error = error;
            std::string rollback_error;
            page.erase(*free_slot, rollback_error);
            std::memcpy(slot_bytes, previous_bytes.data(), record_size_);
            release(manager_, frame, false);
            if (reserved_tail) {
                release(manager_, *reserved_tail, false);
            }
            error = transition_error;
            return false;
        }
    }
    row_id = RowId{
        page_id,
        static_cast<std::uint32_t>(*free_slot)
    };
    release(manager_, frame, true);
    if (reserved_tail) {
        release(manager_, *reserved_tail, false);
    }
    return true;
}

bool HeapFile::pin_record_page(const RowId row_id, SlottedPage& page,
                               std::size_t& frame, std::string& error) {
    // TODO(student): shared path for find/update/erase — reject the header
    // page and pages beyond the file's extent (resident pages count as
    // existing even before write-back), pin, attach, and require the slot
    // occupied. The caller owns the pin on success.
    if (row_id.page_id == 0) {
        error = "page 0 is the header page; it never stores records";
        return false;
    }
    const std::uint64_t blocks = bufman::block_count(file_, error);
    if (!error.empty()) {
        return false;
    }
    if (row_id.page_id >= blocks && !manager_.resident(file_, row_id.page_id)) {
        error = "page " + std::to_string(row_id.page_id) + " is beyond the end of the file";
        return false;
    }
    if (!manager_.pin(file_, row_id.page_id, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer, record_size_);
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    if (!page.is_occupied(row_id.slot, error)) {
        if (error.empty()) {
            error = "no record at row id (page " + std::to_string(row_id.page_id) + ", slot " +
                std::to_string(row_id.slot) + "): the slot is free";
        }
        release(manager_, frame, false);
        return false;
    }
    return true;
}

bool HeapFile::find(const RowId row_id, tuple::Tuple& tuple,
                    std::string& error) {
    // TODO(student): pin the record page, copy the record out, unpin, and
    // deserialize into `tuple`.
    if (!require_open(error)) {
        return false;
    }
    std::size_t frame = 0;
    SlottedPage page;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    std::array<char, bufman::kPageSize> record{};
    const bool got = page.get_record(row_id.slot, record.data(), record.size(), error);
    release(manager_, frame, false);
    if (!got) {
        return false;
    }
    return serializer_->deserialize(record.data(), record.size(), tuple, error);
}

bool HeapFile::update(const RowId row_id, const tuple::Tuple& tuple,
                      std::string& error) {
    // TODO(student): serialize, pin the record page, overwrite the record
    // in place, and unpin. Occupancy never changes, so no list move.
    if (!require_open(error)) {
        return false;
    }
    std::array<char, bufman::kPageSize> record{};
    if (!serializer_->serialize(tuple, record.data(), record.size(), error)) {
        return false;
    }
    std::size_t frame = 0;
    SlottedPage page;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    const bool ok = page.put_record(row_id.slot, record.data(), record_size_, error);
    release(manager_, frame, ok);
    return ok;
}

bool HeapFile::erase(const RowId row_id, std::string& error) {
    // TODO(student): pin the record page, note whether the page WAS full,
    // clear the slot's occupancy byte, and move a just-unfilled page back
    // to the free list (at the tail).
    if (!require_open(error)) {
        return false;
    }
    std::size_t frame = 0;
    SlottedPage page;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    const bool was_full = (page.occupied_count() == page.capacity());
    std::vector<char> original;
    if (was_full) {
        original.resize(record_size_);
        if (!page.get_record(row_id.slot, original.data(), original.size(), error)) {
            release(manager_, frame, false);
            return false;
        }
    }
    const bool done = page.erase(row_id.slot, error);
    if (!done) {
        release(manager_, frame, false);
        return false;
    }
    if (was_full) {
        if (!move_between_lists(false, row_id.page_id, frame, error)) {
            const std::string move_error = error;
            std::string restore_error;
            page.put_record(row_id.slot, original.data(), original.size(), restore_error);
            release(manager_, frame, false);
            error = move_error;
            return false;
        }
    }
    release(manager_, frame, true);
    return true;
}

// --- Structure self-check ---------------------------------------------------

bool HeapFile::check_structure(std::string& error) {
    // TODO(student): walk both lists one pin at a time and verify every
    // invariant the header documents: heads/tails agree with the walk,
    // every allocated page reachable exactly once, membership matches
    // occupancy, page 0 on no list, the union covering exactly pages
    error.clear();
    if (!require_open(error)) {
        return false;
    }
    const std::uint64_t blocks =
        bufman::block_count(file_, error);
    if (!error.empty()) {
        return false;
    }
    if (blocks == 0) {
        error = "heap file has no header page";
        return false;
    }

    std::size_t header_frame = 0;
    if (!manager_.pin(file_, 0, header_frame, error)) {
        return false;
    }

    SlottedPage header;
    header.bind(manager_.frame(header_frame).buffer,
                record_size_);
    if (!header.attach(error)) {
        release(manager_, header_frame, false);
        return false;
    }

    const HeaderFields fields =
        read_header_fields(manager_.frame(header_frame).buffer);
    release(manager_, header_frame, false);
    if (!header_fields_in_range(fields, blocks, error)) {
        return false;
    }
    if ((fields.free_head == kNoPage) != (fields.free_tail == kNoPage) ||
        (fields.full_head == kNoPage) != (fields.full_tail == kNoPage)) {
        error = "heap-file list head/tail mismatch";
        return false;
    }

    if (blocks > fields.next_page_id) {
        error = "on-disk data pages exist outside the allocation range";
        return false;
    }

    std::vector<char> seen(fields.next_page_id, 0);
    const std::array<std::uint32_t, 2> heads{
        fields.free_head, fields.full_head
    };
    const std::array<std::uint32_t, 2> tails{
        fields.free_tail, fields.full_tail
    };

    for (std::size_t list_index = 0;
         list_index < 2; ++list_index) {
        const bool checking_free = (list_index == 0);
        const char* list_name = checking_free ? "FREE" : "FULL";
        std::uint32_t current = heads[list_index];
        std::uint32_t expected_prev = kNoPage;
        while (current != kNoPage) {
            if (current == 0) {
                error = "header page 0 cannot belong to a data list";
                return false;
            }
            if (current >= fields.next_page_id) {
                error = "list points to unallocated page " + std::to_string(current);
                return false;
            }
            if (seen[current]) {
                error = "cyclic or duplicate list page " + std::to_string(current);
                return false;
            }

            seen[current] = 1;
            // if (current >= blocks &&
            //     !manager_.resident(file_, current)) {
            //     error = "allocated page " + std::to_string(current) + " is neither on disk nor resident";
            //     return false;
            // }

            std::size_t frame = 0;
            if (!manager_.pin(file_, current, frame, error)) {
                return false;
            }
            SlottedPage page;
            page.bind(manager_.frame(frame).buffer,
                      record_size_);
            if (!page.attach(error)) {
                release(manager_, frame, false);
                return false;
            }

            const std::uint32_t actual_prev = page.prev_page();
            const std::uint32_t actual_next = page.next_page();
            const bool is_full = (page.occupied_count() == page.capacity());
            release(manager_, frame, false);

            if (actual_prev != expected_prev) {
                error = std::string(list_name) + " list: page " +
                std::to_string(current) + " has an incorrect prev link";
                return false;
            }
            if (is_full == checking_free) {
                error = std::string(list_name) + " list: page " +
                std::to_string(current) + " has wrong occupancy";
                return false;
            }
            expected_prev = current;
            current = actual_next;
        }
        if (expected_prev != tails[list_index]) {
            error = std::string(list_name) + " list tail disagrees with walk";
            return false;
        }
    }
    for (std::uint32_t id = 1; id < fields.next_page_id; ++id) {
        if (!seen[id]) {
            error = "allocated page " + std::to_string(id) + " is unreachable from both lists";
            return false;
        }
    }
    return true;
}

// --- Lifecycle --------------------------------------------------------------

bool HeapFile::resident(std::uint32_t page_id) const {
    // TODO(student): observe the manager's pool.
    return open_ && manager_.resident(file_, page_id);
}

bool HeapFile::close(std::string& error) {
    // TODO(student): close the file through the manager; reset this
    // HeapFile's state only when that succeeded, so a failed close can be
    // retried.
    error.clear();
    if (!require_open(error)) {
        return false;
    }
    if (!manager_.close_file(file_, error)) {
        return false;
    }

    file_ = bufman::File{};
    schema_ = nullptr;
    serializer_.reset();
    record_size_ = 0;
    open_ = false;
    return true;
}

// --- HeapFileIterator -------------------------------------------------------

HeapFileIterator::HeapFileIterator(HeapFile& file, std::uint32_t free_head,
                                   std::uint32_t full_head,
                                   std::uint32_t allocated)
    : file_(&file),
      page_id_(free_head),
      next_list_(full_head),
      slot_(0),
      seen_(allocated, 0) {}

bool HeapFileIterator::next(tuple::Tuple& tuple, std::string& error) {
    // TODO(student): walk the free list's pages, then the full list's, in
    // list order; on each page scan slots in order and yield every
    // occupied slot's decoded record, remembering the slot to resume from.
    // Exhaustion is false with an EMPTY error; a revisited page means a
    // cyclic list and is a named failure.
    error.clear();
    if (file_ == nullptr || !file_->is_open()) {
        error = "heap file is not open";
        return false;
    }
    while (true) {
        if (page_id_ == kNoPage) {
            if (next_list_ == kNoPage) {
                return false;  // Normal exhaustion.
            }
            page_id_ = next_list_;
            next_list_ = kNoPage;
            slot_ = 0;
            iterating_ = kNoPage;
        }
        if (page_id_ == 0 || page_id_ >= seen_.size()) {
            error = "scan points to an invalid data page " + std::to_string(page_id_);
            return false;
        }
        if (seen_[page_id_]) {
            error = "cyclic heap-file list at page " + std::to_string(page_id_);
            return false;
        }

        std::size_t frame = 0;
        if (!file_->manager_.pin(file_->file_, page_id_, frame, error)) {
            return false;
        }
        SlottedPage page;
        page.bind(file_->manager_.frame(frame).buffer, file_->record_size_);
        if (!page.attach(error)) {
            release(file_->manager_, frame, false);
            return false;
        }

        iterating_ = page_id_;
        for (std::size_t slot = slot_; slot < page.capacity(); ++slot) {
            const bool occupied = page.is_occupied(slot, error);
            if (!error.empty()) {
                release(file_->manager_, frame, false);
                return false;
            }
            if (!occupied) {
                continue;
            }
            std::array<char, bufman::kPageSize> record{};
            if (!page.get_record(slot, record.data(), record.size(), error)) {
                release(file_->manager_, frame, false);
                return false;
            }
            release(file_->manager_, frame, false);
            tuple::Tuple decoded;
            if (!file_->serializer_->deserialize(record.data(), file_->record_size_, decoded, error)) {
                return false;
            }
            tuple = decoded;

            current_ = RowId{
                page_id_,
                static_cast<std::uint32_t>(slot)
            };
            slot_ = static_cast<std::uint32_t>(slot + 1);
            return true;
        }
        const std::uint32_t following = page.next_page();
        release(file_->manager_, frame, false);
        seen_[page_id_] = 1;
        page_id_ = following;
        slot_ = 0;
        iterating_ = kNoPage;
    }
}

// --- Free functions ---------------------------------------------------------

bool erase_file(bufman::BufferManager& manager, const std::string& path,
                std::string& error) {
    error.clear();
    if (manager.is_open(path)) {
        error = "cannot erase \"" + path + "\": the file is open";
        return false;
    }
    if (std::remove(path.c_str()) != 0) {
        error = "cannot erase \"" + path + "\": " + std::strerror(errno);
        return false;
    }
    return true;
}
}
