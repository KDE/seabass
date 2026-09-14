// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

#include "rekordbox_pdb.h"
#include "kaitai/exceptions.h"
const std::set<rekordbox_pdb_t::page_type_t> rekordbox_pdb_t::_values_page_type_t{
    rekordbox_pdb_t::PAGE_TYPE_TRACKS,
    rekordbox_pdb_t::PAGE_TYPE_GENRES,
    rekordbox_pdb_t::PAGE_TYPE_ARTISTS,
    rekordbox_pdb_t::PAGE_TYPE_ALBUMS,
    rekordbox_pdb_t::PAGE_TYPE_LABELS,
    rekordbox_pdb_t::PAGE_TYPE_KEYS,
    rekordbox_pdb_t::PAGE_TYPE_COLORS,
    rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE,
    rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES,
    rekordbox_pdb_t::PAGE_TYPE_UNKNOWN_9,
    rekordbox_pdb_t::PAGE_TYPE_UNKNOWN_10,
    rekordbox_pdb_t::PAGE_TYPE_HISTORY_PLAYLISTS,
    rekordbox_pdb_t::PAGE_TYPE_HISTORY_ENTRIES,
    rekordbox_pdb_t::PAGE_TYPE_ARTWORK,
    rekordbox_pdb_t::PAGE_TYPE_UNKNOWN_14,
    rekordbox_pdb_t::PAGE_TYPE_UNKNOWN_15,
    rekordbox_pdb_t::PAGE_TYPE_COLUMNS,
    rekordbox_pdb_t::PAGE_TYPE_UNKNOWN_17,
    rekordbox_pdb_t::PAGE_TYPE_UNKNOWN_18,
    rekordbox_pdb_t::PAGE_TYPE_HISTORY,
};
bool rekordbox_pdb_t::_is_defined_page_type_t(rekordbox_pdb_t::page_type_t v) {
    return rekordbox_pdb_t::_values_page_type_t.find(v) != rekordbox_pdb_t::_values_page_type_t.end();
}
const std::set<rekordbox_pdb_t::page_type_ext_t> rekordbox_pdb_t::_values_page_type_ext_t{
    rekordbox_pdb_t::PAGE_TYPE_EXT_UNKNOWN_0,
    rekordbox_pdb_t::PAGE_TYPE_EXT_UNKNOWN_1,
    rekordbox_pdb_t::PAGE_TYPE_EXT_UNKNOWN_2,
    rekordbox_pdb_t::PAGE_TYPE_EXT_TAGS,
    rekordbox_pdb_t::PAGE_TYPE_EXT_TAG_TRACKS,
    rekordbox_pdb_t::PAGE_TYPE_EXT_UNKNOWN_5,
    rekordbox_pdb_t::PAGE_TYPE_EXT_UNKNOWN_6,
    rekordbox_pdb_t::PAGE_TYPE_EXT_UNKNOWN_7,
    rekordbox_pdb_t::PAGE_TYPE_EXT_UNKNOWN_8,
};
bool rekordbox_pdb_t::_is_defined_page_type_ext_t(rekordbox_pdb_t::page_type_ext_t v) {
    return rekordbox_pdb_t::_values_page_type_ext_t.find(v) != rekordbox_pdb_t::_values_page_type_ext_t.end();
}

rekordbox_pdb_t::rekordbox_pdb_t(bool p_is_ext, kaitai::kstream* p__io, kaitai::kstruct* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root ? p__root : this;
    m_is_ext = p_is_ext;
    m_tables = nullptr;
    _read();
}

void rekordbox_pdb_t::_read() {
    m__unnamed0 = m__io->read_u4le();
    m_len_page = m__io->read_u4le();
    m_num_tables = m__io->read_u4le();
    m_next_unused_page = m__io->read_u4le();
    m__unnamed4 = m__io->read_u4le();
    m_sequence = m__io->read_u4le();
    m_gap = m__io->read_bytes(4);
    if (!(m_gap == std::string("\x00\x00\x00\x00", 4))) {
        throw kaitai::validation_not_equal_error<std::string>(std::string("\x00\x00\x00\x00", 4), m_gap, m__io, std::string("/seq/6"));
    }
    m_tables = std::unique_ptr<std::vector<std::unique_ptr<table_t>>>(new std::vector<std::unique_ptr<table_t>>());
    const int l_tables = num_tables();
    for (int i = 0; i < l_tables; i++) {
        m_tables->push_back(std::move(std::unique_ptr<table_t>(new table_t(m__io, this, m__root))));
    }
}

rekordbox_pdb_t::~rekordbox_pdb_t() {
    _clean_up();
}

void rekordbox_pdb_t::_clean_up() {
}

rekordbox_pdb_t::album_row_t::album_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    f_name = false;
    f_ofs_name_far = false;
    _read();
}

void rekordbox_pdb_t::album_row_t::_read() {
    m_subtype = m__io->read_u2le();
    m_index_shift = m__io->read_u2le();
    m__unnamed2 = m__io->read_u4le();
    m_artist_id = m__io->read_u4le();
    m_id = m__io->read_u4le();
    m__unnamed5 = m__io->read_u4le();
    m__unnamed6 = m__io->read_u1();
    m_ofs_name_near = m__io->read_u1();
}

rekordbox_pdb_t::album_row_t::~album_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::album_row_t::_clean_up() {
    if (f_name) {
    }
    if (f_ofs_name_far && !n_ofs_name_far) {
    }
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::album_row_t::name() {
    if (f_name)
        return m_name.get();
    f_name = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + (((subtype() & 4) == 4) ? (ofs_name_far()) : (ofs_name_near())));
    m_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_name.get();
}

uint16_t rekordbox_pdb_t::album_row_t::ofs_name_far() {
    if (f_ofs_name_far)
        return m_ofs_name_far;
    f_ofs_name_far = true;
    n_ofs_name_far = true;
    if ((subtype() & 4) == 4) {
        n_ofs_name_far = false;
        std::streampos _pos = m__io->pos();
        m__io->seek(_parent()->row_base() + 22);
        m_ofs_name_far = m__io->read_u2le();
        m__io->seek(_pos);
    }
    return m_ofs_name_far;
}

rekordbox_pdb_t::artist_row_t::artist_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    f_name = false;
    f_ofs_name_far = false;
    _read();
}

void rekordbox_pdb_t::artist_row_t::_read() {
    m_subtype = m__io->read_u2le();
    m_index_shift = m__io->read_u2le();
    m_id = m__io->read_u4le();
    m__unnamed3 = m__io->read_u1();
    m_ofs_name_near = m__io->read_u1();
}

rekordbox_pdb_t::artist_row_t::~artist_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::artist_row_t::_clean_up() {
    if (f_name) {
    }
    if (f_ofs_name_far && !n_ofs_name_far) {
    }
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::artist_row_t::name() {
    if (f_name)
        return m_name.get();
    f_name = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + (((subtype() & 4) == 4) ? (ofs_name_far()) : (ofs_name_near())));
    m_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_name.get();
}

uint16_t rekordbox_pdb_t::artist_row_t::ofs_name_far() {
    if (f_ofs_name_far)
        return m_ofs_name_far;
    f_ofs_name_far = true;
    n_ofs_name_far = true;
    if ((subtype() & 4) == 4) {
        n_ofs_name_far = false;
        std::streampos _pos = m__io->pos();
        m__io->seek(_parent()->row_base() + 10);
        m_ofs_name_far = m__io->read_u2le();
        m__io->seek(_pos);
    }
    return m_ofs_name_far;
}

rekordbox_pdb_t::artwork_row_t::artwork_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_path = nullptr;
    _read();
}

void rekordbox_pdb_t::artwork_row_t::_read() {
    m_id = m__io->read_u4le();
    m_path = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
}

rekordbox_pdb_t::artwork_row_t::~artwork_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::artwork_row_t::_clean_up() {
}

rekordbox_pdb_t::color_row_t::color_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    _read();
}

void rekordbox_pdb_t::color_row_t::_read() {
    m__unnamed0 = m__io->read_bytes(5);
    m_id = m__io->read_u2le();
    m__unnamed2 = m__io->read_u1();
    m_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
}

rekordbox_pdb_t::color_row_t::~color_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::color_row_t::_clean_up() {
}

rekordbox_pdb_t::device_sql_long_ascii_t::device_sql_long_ascii_t(kaitai::kstream* p__io, rekordbox_pdb_t::device_sql_string_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void rekordbox_pdb_t::device_sql_long_ascii_t::_read() {
    m_length = m__io->read_u2le();
    m__unnamed1 = m__io->read_u1();
    m_text = kaitai::kstream::bytes_to_str(m__io->read_bytes(length() - 4), "ASCII");
}

rekordbox_pdb_t::device_sql_long_ascii_t::~device_sql_long_ascii_t() {
    _clean_up();
}

void rekordbox_pdb_t::device_sql_long_ascii_t::_clean_up() {
}

rekordbox_pdb_t::device_sql_long_utf16le_t::device_sql_long_utf16le_t(kaitai::kstream* p__io, rekordbox_pdb_t::device_sql_string_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void rekordbox_pdb_t::device_sql_long_utf16le_t::_read() {
    m_length = m__io->read_u2le();
    m__unnamed1 = m__io->read_u1();
    m_text = kaitai::kstream::bytes_to_str(m__io->read_bytes(length() - 4), "UTF-16LE");
}

rekordbox_pdb_t::device_sql_long_utf16le_t::~device_sql_long_utf16le_t() {
    _clean_up();
}

void rekordbox_pdb_t::device_sql_long_utf16le_t::_clean_up() {
}

rekordbox_pdb_t::device_sql_short_ascii_t::device_sql_short_ascii_t(uint8_t p_length_and_kind, kaitai::kstream* p__io, rekordbox_pdb_t::device_sql_string_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_length_and_kind = p_length_and_kind;
    f_length = false;
    _read();
}

void rekordbox_pdb_t::device_sql_short_ascii_t::_read() {
    m_text = kaitai::kstream::bytes_to_str(m__io->read_bytes(length() - 1), "ASCII");
}

rekordbox_pdb_t::device_sql_short_ascii_t::~device_sql_short_ascii_t() {
    _clean_up();
}

void rekordbox_pdb_t::device_sql_short_ascii_t::_clean_up() {
}

int32_t rekordbox_pdb_t::device_sql_short_ascii_t::length() {
    if (f_length)
        return m_length;
    f_length = true;
    m_length = length_and_kind() >> 1;
    return m_length;
}

rekordbox_pdb_t::device_sql_string_t::device_sql_string_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void rekordbox_pdb_t::device_sql_string_t::_read() {
    m_length_and_kind = m__io->read_u1();
    switch (length_and_kind()) {
    case 144: {
        m_body = std::unique_ptr<device_sql_long_utf16le_t>(new device_sql_long_utf16le_t(m__io, this, m__root));
        break;
    }
    case 64: {
        m_body = std::unique_ptr<device_sql_long_ascii_t>(new device_sql_long_ascii_t(m__io, this, m__root));
        break;
    }
    default: {
        m_body = std::unique_ptr<device_sql_short_ascii_t>(new device_sql_short_ascii_t(length_and_kind(), m__io, this, m__root));
        break;
    }
    }
}

rekordbox_pdb_t::device_sql_string_t::~device_sql_string_t() {
    _clean_up();
}

void rekordbox_pdb_t::device_sql_string_t::_clean_up() {
}

rekordbox_pdb_t::genre_row_t::genre_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    _read();
}

void rekordbox_pdb_t::genre_row_t::_read() {
    m_id = m__io->read_u4le();
    m_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
}

rekordbox_pdb_t::genre_row_t::~genre_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::genre_row_t::_clean_up() {
}

rekordbox_pdb_t::history_entry_row_t::history_entry_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void rekordbox_pdb_t::history_entry_row_t::_read() {
    m_track_id = m__io->read_u4le();
    m_playlist_id = m__io->read_u4le();
    m_entry_index = m__io->read_u4le();
}

rekordbox_pdb_t::history_entry_row_t::~history_entry_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::history_entry_row_t::_clean_up() {
}

rekordbox_pdb_t::history_playlist_row_t::history_playlist_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    _read();
}

void rekordbox_pdb_t::history_playlist_row_t::_read() {
    m_id = m__io->read_u4le();
    m_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
}

rekordbox_pdb_t::history_playlist_row_t::~history_playlist_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::history_playlist_row_t::_clean_up() {
}

rekordbox_pdb_t::key_row_t::key_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    _read();
}

void rekordbox_pdb_t::key_row_t::_read() {
    m_id = m__io->read_u4le();
    m_id2 = m__io->read_u4le();
    m_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
}

rekordbox_pdb_t::key_row_t::~key_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::key_row_t::_clean_up() {
}

rekordbox_pdb_t::label_row_t::label_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    _read();
}

void rekordbox_pdb_t::label_row_t::_read() {
    m_id = m__io->read_u4le();
    m_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
}

rekordbox_pdb_t::label_row_t::~label_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::label_row_t::_clean_up() {
}

rekordbox_pdb_t::page_t::page_t(kaitai::kstream* p__io, rekordbox_pdb_t::page_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_next_page = nullptr;
    m_row_groups = nullptr;
    f_heap_pos = false;
    f_is_data_page = false;
    f_num_row_groups = false;
    f_row_groups = false;
    _read();
}

void rekordbox_pdb_t::page_t::_read() {
    m_gap = m__io->read_bytes(4);
    if (!(m_gap == std::string("\x00\x00\x00\x00", 4))) {
        throw kaitai::validation_not_equal_error<std::string>(std::string("\x00\x00\x00\x00", 4), m_gap, m__io, std::string("/types/page/seq/0"));
    }
    m_page_index = m__io->read_u4le();
    n_type = true;
    if (!(_root()->is_ext())) {
        n_type = false;
        m_type = static_cast<rekordbox_pdb_t::page_type_t>(m__io->read_u4le());
    }
    n_type_ext = true;
    if (_root()->is_ext()) {
        n_type_ext = false;
        m_type_ext = static_cast<rekordbox_pdb_t::page_type_ext_t>(m__io->read_u4le());
    }
    m_next_page = std::unique_ptr<page_ref_t>(new page_ref_t(m__io, this, m__root));
    m_sequence = m__io->read_u4le();
    m__unnamed6 = m__io->read_bytes(4);
    m_num_row_offsets = m__io->read_bits_int_le(13);
    m_num_rows = m__io->read_bits_int_le(11);
    m__io->align_to_byte();
    m_page_flags = m__io->read_u1();
    m_free_size = m__io->read_u2le();
    m_used_size = m__io->read_u2le();
    m_transaction_row_count = m__io->read_u2le();
    m_transaction_row_index = m__io->read_u2le();
    m__unnamed14 = m__io->read_u2le();
    m__unnamed15 = m__io->read_u2le();
    n_heap = true;
    if (false) {
        n_heap = false;
        m_heap = m__io->read_bytes_full();
    }
}

rekordbox_pdb_t::page_t::~page_t() {
    _clean_up();
}

void rekordbox_pdb_t::page_t::_clean_up() {
    if (!n_type) {
    }
    if (!n_type_ext) {
    }
    if (!n_heap) {
    }
    if (f_row_groups && !n_row_groups) {
    }
}

int32_t rekordbox_pdb_t::page_t::heap_pos() {
    if (f_heap_pos)
        return m_heap_pos;
    f_heap_pos = true;
    m_heap_pos = _io()->pos();
    return m_heap_pos;
}

bool rekordbox_pdb_t::page_t::is_data_page() {
    if (f_is_data_page)
        return m_is_data_page;
    f_is_data_page = true;
    m_is_data_page = (page_flags() & 64) == 0;
    return m_is_data_page;
}

int32_t rekordbox_pdb_t::page_t::num_row_groups() {
    if (f_num_row_groups)
        return m_num_row_groups;
    f_num_row_groups = true;
    m_num_row_groups = (num_row_offsets() - 1) / 16 + 1;
    return m_num_row_groups;
}

std::vector<std::unique_ptr<rekordbox_pdb_t::row_group_t>>* rekordbox_pdb_t::page_t::row_groups() {
    if (f_row_groups)
        return m_row_groups.get();
    f_row_groups = true;
    n_row_groups = true;
    if (is_data_page()) {
        n_row_groups = false;
        m_row_groups = std::unique_ptr<std::vector<std::unique_ptr<row_group_t>>>(new std::vector<std::unique_ptr<row_group_t>>());
        const int l_row_groups = num_row_groups();
        for (int i = 0; i < l_row_groups; i++) {
            m_row_groups->push_back(std::move(std::unique_ptr<row_group_t>(new row_group_t(i, m__io, this, m__root))));
        }
    }
    return m_row_groups.get();
}

rekordbox_pdb_t::page_ref_t::page_ref_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_body = nullptr;
    m__io__raw_body = nullptr;
    f_body = false;
    _read();
}

void rekordbox_pdb_t::page_ref_t::_read() {
    m_index = m__io->read_u4le();
}

rekordbox_pdb_t::page_ref_t::~page_ref_t() {
    _clean_up();
}

void rekordbox_pdb_t::page_ref_t::_clean_up() {
    if (f_body) {
    }
}

rekordbox_pdb_t::page_t* rekordbox_pdb_t::page_ref_t::body() {
    if (f_body)
        return m_body.get();
    f_body = true;
    kaitai::kstream *io = _root()->_io();
    std::streampos _pos = io->pos();
    io->seek(_root()->len_page() * index());
    m__raw_body = io->read_bytes(_root()->len_page());
    m__io__raw_body = std::unique_ptr<kaitai::kstream>(new kaitai::kstream(m__raw_body));
    m_body = std::unique_ptr<page_t>(new page_t(m__io__raw_body.get(), this, m__root));
    io->seek(_pos);
    return m_body.get();
}

rekordbox_pdb_t::playlist_entry_row_t::playlist_entry_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void rekordbox_pdb_t::playlist_entry_row_t::_read() {
    m_entry_index = m__io->read_u4le();
    m_track_id = m__io->read_u4le();
    m_playlist_id = m__io->read_u4le();
}

rekordbox_pdb_t::playlist_entry_row_t::~playlist_entry_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::playlist_entry_row_t::_clean_up() {
}

rekordbox_pdb_t::playlist_tree_row_t::playlist_tree_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    f_is_folder = false;
    _read();
}

void rekordbox_pdb_t::playlist_tree_row_t::_read() {
    m_parent_id = m__io->read_u4le();
    m__unnamed1 = m__io->read_bytes(4);
    m_sort_order = m__io->read_u4le();
    m_id = m__io->read_u4le();
    m_raw_is_folder = m__io->read_u4le();
    m_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
}

rekordbox_pdb_t::playlist_tree_row_t::~playlist_tree_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::playlist_tree_row_t::_clean_up() {
}

bool rekordbox_pdb_t::playlist_tree_row_t::is_folder() {
    if (f_is_folder)
        return m_is_folder;
    f_is_folder = true;
    m_is_folder = raw_is_folder() != 0;
    return m_is_folder;
}

rekordbox_pdb_t::row_group_t::row_group_t(uint16_t p_group_index, kaitai::kstream* p__io, rekordbox_pdb_t::page_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_group_index = p_group_index;
    m_rows = nullptr;
    f_base = false;
    f_row_present_flags = false;
    f_rows = false;
    f_transaction_row_flags = false;
    _read();
}

void rekordbox_pdb_t::row_group_t::_read() {
}

rekordbox_pdb_t::row_group_t::~row_group_t() {
    _clean_up();
}

void rekordbox_pdb_t::row_group_t::_clean_up() {
    if (f_row_present_flags) {
    }
    if (f_rows) {
    }
    if (f_transaction_row_flags) {
    }
}

int32_t rekordbox_pdb_t::row_group_t::base() {
    if (f_base)
        return m_base;
    f_base = true;
    m_base = _root()->len_page() - group_index() * 36;
    return m_base;
}

uint16_t rekordbox_pdb_t::row_group_t::row_present_flags() {
    if (f_row_present_flags)
        return m_row_present_flags;
    f_row_present_flags = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(base() - 4);
    m_row_present_flags = m__io->read_u2le();
    m__io->seek(_pos);
    return m_row_present_flags;
}

std::vector<std::unique_ptr<rekordbox_pdb_t::row_ref_t>>* rekordbox_pdb_t::row_group_t::rows() {
    if (f_rows)
        return m_rows.get();
    f_rows = true;
    m_rows = std::unique_ptr<std::vector<std::unique_ptr<row_ref_t>>>(new std::vector<std::unique_ptr<row_ref_t>>());
    const int l_rows = 16;
    for (int i = 0; i < l_rows; i++) {
        m_rows->push_back(std::move(std::unique_ptr<row_ref_t>(new row_ref_t(i, m__io, this, m__root))));
    }
    return m_rows.get();
}

uint16_t rekordbox_pdb_t::row_group_t::transaction_row_flags() {
    if (f_transaction_row_flags)
        return m_transaction_row_flags;
    f_transaction_row_flags = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(base());
    m_transaction_row_flags = m__io->read_u2le();
    m__io->seek(_pos);
    return m_transaction_row_flags;
}

rekordbox_pdb_t::row_ref_t::row_ref_t(uint16_t p_row_index, kaitai::kstream* p__io, rekordbox_pdb_t::row_group_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_row_index = p_row_index;
    f_body = false;
    f_body_ext = false;
    f_ofs_row = false;
    f_present = false;
    f_row_base = false;
    _read();
}

void rekordbox_pdb_t::row_ref_t::_read() {
}

rekordbox_pdb_t::row_ref_t::~row_ref_t() {
    _clean_up();
}

void rekordbox_pdb_t::row_ref_t::_clean_up() {
    if (f_body && !n_body) {
    }
    if (f_body_ext && !n_body_ext) {
    }
    if (f_ofs_row) {
    }
}

kaitai::kstruct* rekordbox_pdb_t::row_ref_t::body() {
    if (f_body)
        return m_body.get();
    f_body = true;
    n_body = true;
    if ( ((present()) && (!(_root()->is_ext()))) ) {
        n_body = false;
        std::streampos _pos = m__io->pos();
        m__io->seek(row_base());
        n_body = true;
        switch (_parent()->_parent()->type()) {
        case rekordbox_pdb_t::PAGE_TYPE_ALBUMS: {
            n_body = false;
            m_body = std::unique_ptr<album_row_t>(new album_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_ARTISTS: {
            n_body = false;
            m_body = std::unique_ptr<artist_row_t>(new artist_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_ARTWORK: {
            n_body = false;
            m_body = std::unique_ptr<artwork_row_t>(new artwork_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_COLORS: {
            n_body = false;
            m_body = std::unique_ptr<color_row_t>(new color_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_GENRES: {
            n_body = false;
            m_body = std::unique_ptr<genre_row_t>(new genre_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_HISTORY_ENTRIES: {
            n_body = false;
            m_body = std::unique_ptr<history_entry_row_t>(new history_entry_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_HISTORY_PLAYLISTS: {
            n_body = false;
            m_body = std::unique_ptr<history_playlist_row_t>(new history_playlist_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_KEYS: {
            n_body = false;
            m_body = std::unique_ptr<key_row_t>(new key_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_LABELS: {
            n_body = false;
            m_body = std::unique_ptr<label_row_t>(new label_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES: {
            n_body = false;
            m_body = std::unique_ptr<playlist_entry_row_t>(new playlist_entry_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE: {
            n_body = false;
            m_body = std::unique_ptr<playlist_tree_row_t>(new playlist_tree_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_TRACKS: {
            n_body = false;
            m_body = std::unique_ptr<track_row_t>(new track_row_t(m__io, this, m__root));
            break;
        }
        }
        m__io->seek(_pos);
    }
    return m_body.get();
}

kaitai::kstruct* rekordbox_pdb_t::row_ref_t::body_ext() {
    if (f_body_ext)
        return m_body_ext.get();
    f_body_ext = true;
    n_body_ext = true;
    if ( ((present()) && (_root()->is_ext())) ) {
        n_body_ext = false;
        std::streampos _pos = m__io->pos();
        m__io->seek(row_base());
        n_body_ext = true;
        switch (_parent()->_parent()->type_ext()) {
        case rekordbox_pdb_t::PAGE_TYPE_EXT_TAG_TRACKS: {
            n_body_ext = false;
            m_body_ext = std::unique_ptr<tag_track_row_t>(new tag_track_row_t(m__io, this, m__root));
            break;
        }
        case rekordbox_pdb_t::PAGE_TYPE_EXT_TAGS: {
            n_body_ext = false;
            m_body_ext = std::unique_ptr<tag_row_t>(new tag_row_t(m__io, this, m__root));
            break;
        }
        }
        m__io->seek(_pos);
    }
    return m_body_ext.get();
}

uint16_t rekordbox_pdb_t::row_ref_t::ofs_row() {
    if (f_ofs_row)
        return m_ofs_row;
    f_ofs_row = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->base() - (6 + 2 * row_index()));
    m_ofs_row = m__io->read_u2le();
    m__io->seek(_pos);
    return m_ofs_row;
}

bool rekordbox_pdb_t::row_ref_t::present() {
    if (f_present)
        return m_present;
    f_present = true;
    m_present = (((_parent()->row_present_flags() >> row_index() & 1) != 0) ? (true) : (false));
    return m_present;
}

int32_t rekordbox_pdb_t::row_ref_t::row_base() {
    if (f_row_base)
        return m_row_base;
    f_row_base = true;
    m_row_base = ofs_row() + _parent()->_parent()->heap_pos();
    return m_row_base;
}

rekordbox_pdb_t::table_t::table_t(kaitai::kstream* p__io, rekordbox_pdb_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_first_page = nullptr;
    m_last_page = nullptr;
    _read();
}

void rekordbox_pdb_t::table_t::_read() {
    n_type = true;
    if (!(_root()->is_ext())) {
        n_type = false;
        m_type = static_cast<rekordbox_pdb_t::page_type_t>(m__io->read_u4le());
    }
    n_type_ext = true;
    if (_root()->is_ext()) {
        n_type_ext = false;
        m_type_ext = static_cast<rekordbox_pdb_t::page_type_ext_t>(m__io->read_u4le());
    }
    m_empty_candidate = m__io->read_u4le();
    m_first_page = std::unique_ptr<page_ref_t>(new page_ref_t(m__io, this, m__root));
    m_last_page = std::unique_ptr<page_ref_t>(new page_ref_t(m__io, this, m__root));
}

rekordbox_pdb_t::table_t::~table_t() {
    _clean_up();
}

void rekordbox_pdb_t::table_t::_clean_up() {
    if (!n_type) {
    }
    if (!n_type_ext) {
    }
}

rekordbox_pdb_t::tag_row_t::tag_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_name = nullptr;
    f_is_category = false;
    f_name = false;
    f_ofs_name_far = false;
    _read();
}

void rekordbox_pdb_t::tag_row_t::_read() {
    m_subtype = m__io->read_u2le();
    m_tag_index = m__io->read_u2le();
    m__unnamed2 = m__io->read_u8le();
    m_category = m__io->read_u4le();
    m_category_pos = m__io->read_u4le();
    m_id = m__io->read_u4le();
    m_raw_is_category = m__io->read_u4le();
    m__unnamed7 = m__io->read_u1();
    m_ofs_name_near = m__io->read_u1();
    m_ofs_unknown_near = m__io->read_u1();
}

rekordbox_pdb_t::tag_row_t::~tag_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::tag_row_t::_clean_up() {
    if (f_name) {
    }
    if (f_ofs_name_far && !n_ofs_name_far) {
    }
}

bool rekordbox_pdb_t::tag_row_t::is_category() {
    if (f_is_category)
        return m_is_category;
    f_is_category = true;
    m_is_category = raw_is_category() != 0;
    return m_is_category;
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::tag_row_t::name() {
    if (f_name)
        return m_name.get();
    f_name = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + (((subtype() & 4) == 4) ? (ofs_name_far()) : (ofs_name_near())));
    m_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_name.get();
}

uint16_t rekordbox_pdb_t::tag_row_t::ofs_name_far() {
    if (f_ofs_name_far)
        return m_ofs_name_far;
    f_ofs_name_far = true;
    n_ofs_name_far = true;
    if ((subtype() & 4) == 4) {
        n_ofs_name_far = false;
        std::streampos _pos = m__io->pos();
        m__io->seek(_parent()->row_base() + 30);
        m_ofs_name_far = m__io->read_u2le();
        m__io->seek(_pos);
    }
    return m_ofs_name_far;
}

rekordbox_pdb_t::tag_track_row_t::tag_track_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    _read();
}

void rekordbox_pdb_t::tag_track_row_t::_read() {
    m__unnamed0 = m__io->read_u4le();
    m_track_id = m__io->read_u4le();
    m_tag_id = m__io->read_u4le();
    m__unnamed3 = m__io->read_u4le();
}

rekordbox_pdb_t::tag_track_row_t::~tag_track_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::tag_track_row_t::_clean_up() {
}

rekordbox_pdb_t::track_row_t::track_row_t(kaitai::kstream* p__io, rekordbox_pdb_t::row_ref_t* p__parent, rekordbox_pdb_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_ofs_strings = nullptr;
    m_analyze_date = nullptr;
    m_analyze_path = nullptr;
    m_autoload_hot_cues = nullptr;
    m_comment = nullptr;
    m_date_added = nullptr;
    m_file_path = nullptr;
    m_filename = nullptr;
    m_isrc = nullptr;
    m_kuvo_public = nullptr;
    m_message = nullptr;
    m_mix_name = nullptr;
    m_release_date = nullptr;
    m_texter = nullptr;
    m_title = nullptr;
    m_unknown_string_2 = nullptr;
    m_unknown_string_3 = nullptr;
    m_unknown_string_4 = nullptr;
    m_unknown_string_5 = nullptr;
    m_unknown_string_6 = nullptr;
    m_unknown_string_7 = nullptr;
    m_unknown_string_8 = nullptr;
    f_analyze_date = false;
    f_analyze_path = false;
    f_autoload_hot_cues = false;
    f_comment = false;
    f_date_added = false;
    f_file_path = false;
    f_filename = false;
    f_isrc = false;
    f_kuvo_public = false;
    f_message = false;
    f_mix_name = false;
    f_release_date = false;
    f_texter = false;
    f_title = false;
    f_unknown_string_2 = false;
    f_unknown_string_3 = false;
    f_unknown_string_4 = false;
    f_unknown_string_5 = false;
    f_unknown_string_6 = false;
    f_unknown_string_7 = false;
    f_unknown_string_8 = false;
    _read();
}

void rekordbox_pdb_t::track_row_t::_read() {
    m_subtype = m__io->read_u2le();
    m_index_shift = m__io->read_u2le();
    m_bitmask = m__io->read_u4le();
    m_sample_rate = m__io->read_u4le();
    m_composer_id = m__io->read_u4le();
    m_file_size = m__io->read_u4le();
    m__unnamed6 = m__io->read_u4le();
    m__unnamed7 = m__io->read_u2le();
    m__unnamed8 = m__io->read_u2le();
    m_artwork_id = m__io->read_u4le();
    m_key_id = m__io->read_u4le();
    m_original_artist_id = m__io->read_u4le();
    m_label_id = m__io->read_u4le();
    m_remixer_id = m__io->read_u4le();
    m_bitrate = m__io->read_u4le();
    m_track_number = m__io->read_u4le();
    m_tempo = m__io->read_u4le();
    m_genre_id = m__io->read_u4le();
    m_album_id = m__io->read_u4le();
    m_artist_id = m__io->read_u4le();
    m_id = m__io->read_u4le();
    m_disc_number = m__io->read_u2le();
    m_play_count = m__io->read_u2le();
    m_year = m__io->read_u2le();
    m_sample_depth = m__io->read_u2le();
    m_duration = m__io->read_u2le();
    m__unnamed26 = m__io->read_u2le();
    m_color_id = m__io->read_u1();
    m_rating = m__io->read_u1();
    m__unnamed29 = m__io->read_u2le();
    m__unnamed30 = m__io->read_u2le();
    m_ofs_strings = std::unique_ptr<std::vector<uint16_t>>(new std::vector<uint16_t>());
    const int l_ofs_strings = 21;
    for (int i = 0; i < l_ofs_strings; i++) {
        m_ofs_strings->push_back(std::move(m__io->read_u2le()));
    }
}

rekordbox_pdb_t::track_row_t::~track_row_t() {
    _clean_up();
}

void rekordbox_pdb_t::track_row_t::_clean_up() {
    if (f_analyze_date) {
    }
    if (f_analyze_path) {
    }
    if (f_autoload_hot_cues) {
    }
    if (f_comment) {
    }
    if (f_date_added) {
    }
    if (f_file_path) {
    }
    if (f_filename) {
    }
    if (f_isrc) {
    }
    if (f_kuvo_public) {
    }
    if (f_message) {
    }
    if (f_mix_name) {
    }
    if (f_release_date) {
    }
    if (f_texter) {
    }
    if (f_title) {
    }
    if (f_unknown_string_2) {
    }
    if (f_unknown_string_3) {
    }
    if (f_unknown_string_4) {
    }
    if (f_unknown_string_5) {
    }
    if (f_unknown_string_6) {
    }
    if (f_unknown_string_7) {
    }
    if (f_unknown_string_8) {
    }
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::analyze_date() {
    if (f_analyze_date)
        return m_analyze_date.get();
    f_analyze_date = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(15));
    m_analyze_date = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_analyze_date.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::analyze_path() {
    if (f_analyze_path)
        return m_analyze_path.get();
    f_analyze_path = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(14));
    m_analyze_path = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_analyze_path.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::autoload_hot_cues() {
    if (f_autoload_hot_cues)
        return m_autoload_hot_cues.get();
    f_autoload_hot_cues = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(7));
    m_autoload_hot_cues = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_autoload_hot_cues.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::comment() {
    if (f_comment)
        return m_comment.get();
    f_comment = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(16));
    m_comment = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_comment.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::date_added() {
    if (f_date_added)
        return m_date_added.get();
    f_date_added = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(10));
    m_date_added = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_date_added.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::file_path() {
    if (f_file_path)
        return m_file_path.get();
    f_file_path = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(20));
    m_file_path = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_file_path.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::filename() {
    if (f_filename)
        return m_filename.get();
    f_filename = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(19));
    m_filename = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_filename.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::isrc() {
    if (f_isrc)
        return m_isrc.get();
    f_isrc = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(0));
    m_isrc = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_isrc.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::kuvo_public() {
    if (f_kuvo_public)
        return m_kuvo_public.get();
    f_kuvo_public = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(6));
    m_kuvo_public = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_kuvo_public.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::message() {
    if (f_message)
        return m_message.get();
    f_message = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(5));
    m_message = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_message.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::mix_name() {
    if (f_mix_name)
        return m_mix_name.get();
    f_mix_name = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(12));
    m_mix_name = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_mix_name.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::release_date() {
    if (f_release_date)
        return m_release_date.get();
    f_release_date = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(11));
    m_release_date = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_release_date.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::texter() {
    if (f_texter)
        return m_texter.get();
    f_texter = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(1));
    m_texter = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_texter.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::title() {
    if (f_title)
        return m_title.get();
    f_title = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(17));
    m_title = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_title.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::unknown_string_2() {
    if (f_unknown_string_2)
        return m_unknown_string_2.get();
    f_unknown_string_2 = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(2));
    m_unknown_string_2 = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_unknown_string_2.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::unknown_string_3() {
    if (f_unknown_string_3)
        return m_unknown_string_3.get();
    f_unknown_string_3 = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(3));
    m_unknown_string_3 = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_unknown_string_3.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::unknown_string_4() {
    if (f_unknown_string_4)
        return m_unknown_string_4.get();
    f_unknown_string_4 = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(4));
    m_unknown_string_4 = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_unknown_string_4.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::unknown_string_5() {
    if (f_unknown_string_5)
        return m_unknown_string_5.get();
    f_unknown_string_5 = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(8));
    m_unknown_string_5 = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_unknown_string_5.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::unknown_string_6() {
    if (f_unknown_string_6)
        return m_unknown_string_6.get();
    f_unknown_string_6 = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(9));
    m_unknown_string_6 = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_unknown_string_6.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::unknown_string_7() {
    if (f_unknown_string_7)
        return m_unknown_string_7.get();
    f_unknown_string_7 = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(13));
    m_unknown_string_7 = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_unknown_string_7.get();
}

rekordbox_pdb_t::device_sql_string_t* rekordbox_pdb_t::track_row_t::unknown_string_8() {
    if (f_unknown_string_8)
        return m_unknown_string_8.get();
    f_unknown_string_8 = true;
    std::streampos _pos = m__io->pos();
    m__io->seek(_parent()->row_base() + ofs_strings()->at(18));
    m_unknown_string_8 = std::unique_ptr<device_sql_string_t>(new device_sql_string_t(m__io, this, m__root));
    m__io->seek(_pos);
    return m_unknown_string_8.get();
}
