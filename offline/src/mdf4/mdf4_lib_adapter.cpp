#include "ego_offline/mdf4/mdf4_lib_adapter.hpp"

#include <cstring>
#include <cstdio>
#include <ctime>
#include <limits>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace ego_offline::mdf4 {

// ─────────────────────────────────────────────────────────────────────────────
// SHA-256 (FIPS 180-4) — compact, dependency-free
// ─────────────────────────────────────────────────────────────────────────────
namespace {

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

struct Sha256Ctx {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t  blk[64] = {};
    uint64_t bitlen   = 0;
    uint32_t buflen   = 0;

    static uint32_t rotr(uint32_t x,int n){return (x>>n)|(x<<(32-n));}
    static uint32_t ch(uint32_t x,uint32_t y,uint32_t z){return(x&y)^(~x&z);}
    static uint32_t maj(uint32_t x,uint32_t y,uint32_t z){return(x&y)^(x&z)^(y&z);}
    static uint32_t S0(uint32_t x){return rotr(x,2)^rotr(x,13)^rotr(x,22);}
    static uint32_t S1(uint32_t x){return rotr(x,6)^rotr(x,11)^rotr(x,25);}
    static uint32_t s0(uint32_t x){return rotr(x,7)^rotr(x,18)^(x>>3);}
    static uint32_t s1(uint32_t x){return rotr(x,17)^rotr(x,19)^(x>>10);}

    void transform() {
        uint32_t w[64];
        for(int i=0;i<16;++i)
            w[i]=(uint32_t(blk[i*4])<<24)|(uint32_t(blk[i*4+1])<<16)|
                 (uint32_t(blk[i*4+2])<<8)|uint32_t(blk[i*4+3]);
        for(int i=16;i<64;++i) w[i]=s1(w[i-2])+w[i-7]+s0(w[i-15])+w[i-16];
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hv=h[7];
        for(int i=0;i<64;++i){
            uint32_t t1=hv+S1(e)+ch(e,f,g)+SHA256_K[i]+w[i];
            uint32_t t2=S0(a)+maj(a,b,c);
            hv=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;
        h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hv;
    }

    void update(const uint8_t* data, size_t len) {
        for(size_t i=0;i<len;++i){
            blk[buflen++]=data[i];
            bitlen+=8;
            if(buflen==64){transform();buflen=0;}
        }
    }

    std::string hexdigest() {
        // padding
        uint8_t pad[64]={};
        uint32_t rem=buflen;
        pad[rem++]=0x80;
        if(rem>56){
            while(rem<64) pad[rem++]=0;
            std::memcpy(blk,pad,64); transform();
            rem=0;
        }
        uint8_t fin[64]={};
        std::memcpy(fin,pad,rem);
        for(int i=0;i<8;++i) fin[56+i]=uint8_t(bitlen>>(56-8*i));
        std::memcpy(blk,fin,64); transform();

        char hex[65];
        for(int i=0;i<8;++i)
            std::snprintf(hex+i*8,9,"%08x",h[i]);
        hex[64]='\0';
        return hex;
    }
};

std::string sha256_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if(!f) return "";
    Sha256Ctx ctx;
    uint8_t buf[8192];
    while(f){
        f.read(reinterpret_cast<char*>(buf), sizeof(buf));
        auto n = f.gcount();
        if(n>0) ctx.update(buf, static_cast<size_t>(n));
    }
    return ctx.hexdigest();
}

// ─────────────────────────────────────────────────────────────────────────────
// Binary buffer helpers
// ─────────────────────────────────────────────────────────────────────────────

struct Buf {
    std::vector<uint8_t> d;

    size_t pos() const { return d.size(); }

    void u8 (uint8_t  v){d.push_back(v);}
    void u16(uint16_t v){d.push_back(v&0xFF);d.push_back((v>>8)&0xFF);}
    void u32(uint32_t v){for(int i=0;i<4;++i)d.push_back((v>>(8*i))&0xFF);}
    void u64(uint64_t v){for(int i=0;i<8;++i)d.push_back((v>>(8*i))&0xFF);}
    void i16(int16_t  v){u16(static_cast<uint16_t>(v));}
    void f64(double   v){uint64_t b;std::memcpy(&b,&v,8);u64(b);}
    void f32(float    v){uint32_t b;std::memcpy(&b,&v,4);u32(b);}
    void zeros(size_t n){d.insert(d.end(),n,0);}

    void str_pad(const char* s, size_t n){
        size_t len=std::strlen(s);
        for(size_t i=0;i<n;++i) d.push_back(i<len?(uint8_t)s[i]:0);
    }

    void patch64(size_t at, uint64_t v){
        for(int i=0;i<8;++i) d[at+i]=(v>>(8*i))&0xFF;
    }

    // Write block header; returns position of links array start
    size_t blk_hdr(const char id[4], uint64_t len, uint64_t nlinks){
        d.push_back((uint8_t)id[0]); d.push_back((uint8_t)id[1]);
        d.push_back((uint8_t)id[2]); d.push_back((uint8_t)id[3]);
        zeros(4);   // reserved
        u64(len);
        u64(nlinks);
        size_t lp = pos();
        zeros(nlinks*8); // link placeholders
        return lp;
    }

    // Write a TXBLOCK; returns file offset of block start
    size_t txblock(const std::string& text){
        size_t start = pos();
        size_t tlen  = text.size()+1; // include null
        size_t tpad  = (tlen+7)&~size_t(7);
        uint64_t blen = 24 + tpad;
        blk_hdr("##TX", blen, 0);
        for(char c:text) d.push_back((uint8_t)c);
        d.push_back(0);
        zeros(tpad - tlen);
        return start;
    }

    // Write an MDBLOCK (XML metadata); ASAM viewers expect ##MD for comment_addr links
    size_t mdblock(const std::string& xml){
        size_t start = pos();
        size_t tlen  = xml.size()+1;
        size_t tpad  = (tlen+7)&~size_t(7);
        uint64_t blen = 24 + tpad;
        blk_hdr("##MD", blen, 0);
        for(char c:xml) d.push_back((uint8_t)c);
        d.push_back(0);
        zeros(tpad - tlen);
        return start;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Channel type helpers
// ─────────────────────────────────────────────────────────────────────────────

struct CtypeInfo {
    uint8_t  cn_data_type; // 0=uint_le,2=int_le,4=float_le,6=double_le
    uint32_t byte_size;
    uint32_t bit_count;
};

CtypeInfo ctype(const std::string& dt){
    if(dt=="float64") return {6,8,64};
    if(dt=="float32") return {4,4,32};
    if(dt=="int16")   return {2,2,16};
    return {0,1,8}; // uint8 default
}

// Record layout: byte offsets for [time, ch0, ch1, ...]
struct Layout {
    std::vector<uint32_t> offsets; // index 0 = time channel
    uint32_t rec_size;
};

Layout make_layout(const ChannelGroup& g){
    Layout lay;
    uint32_t off=0;
    lay.offsets.push_back(off); off+=8; // time = float64
    for(const auto& ch:g.channels){
        lay.offsets.push_back(off);
        off += ctype(ch.data_type).byte_size;
    }
    lay.rec_size = off;
    return lay;
}

// ─────────────────────────────────────────────────────────────────────────────
// MDF4 v4.1 file builder
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> build_mdf4(
    const std::vector<ChannelGroup>& groups,
    const load::SessionBundle&       meta,
    ns_t                             t0_ns)
{
    Buf b;

    // ── IDBLOCK (64 bytes, special format) ───────────────────────────────────
    b.str_pad("MDF     ", 8);
    b.str_pad("4.10    ", 8);
    b.str_pad("ego-offl", 8);
    b.zeros(4);
    b.u16(410); // version_number
    b.zeros(34);
    // total 64 bytes

    // ── HDBLOCK (104 bytes) ──────────────────────────────────────────────────
    size_t hd_pos = b.pos(); // = 64
    size_t hd_lp  = b.blk_hdr("##HD", 104, 6);
    // links (patched later): [first_dg, fh, 0, 0, 0, comment]
    // data (32 bytes):
    b.u64(static_cast<uint64_t>(t0_ns)); // hd_start_time_ns
    b.i16(0);  // tz_offset_min
    b.i16(0);  // dst_offset_min
    b.u8(0);   // time_flags (0 = local)
    b.u8(0);   // time_class
    b.u8(0);   // flags
    b.u8(0);   // reserved
    b.f64(0.0); // start_angle_rad
    b.f64(0.0); // start_distance_m
    (void)hd_pos;

    // ── FHBLOCK (56 bytes) ───────────────────────────────────────────────────
    size_t fh_pos = b.pos();
    size_t fh_lp  = b.blk_hdr("##FH", 56, 2);
    // data (16 bytes):
    b.u64(static_cast<uint64_t>(t0_ns)); // fh_time_ns
    b.i16(0); b.i16(0); b.u8(0); b.zeros(3); // tz, dst, flags, reserved
    b.patch64(hd_lp+8, fh_pos); // hd_link[1] = fh

    // ── MDBLOCK: file comment (HD comment_addr must be ##MD, not ##TX) ───────
    std::ostringstream meta_ss;
    meta_ss << "session_id=" << meta.session.session_id
            << " vehicle_id=" << meta.session.vehicle_id
            << " started_at=" << meta.session.started_at_utc
            << " offline_pipeline_version=" << meta.session.runtime_version;
    if(meta.scenario)
        meta_ss << " scenario_id=" << meta.scenario->scenario_id;

    std::ostringstream md_xml;
    md_xml << "<HDcomment><TX>" << meta_ss.str() << "</TX></HDcomment>";
    size_t md_comment_pos = b.mdblock(md_xml.str());
    b.patch64(hd_lp+40, md_comment_pos); // hd link 6 = comment (MDBLOCK)
    b.patch64(fh_lp+8, md_comment_pos);  // fh link 2 = comment (link 1 = next FH, must stay 0)

    // ── Channel groups ───────────────────────────────────────────────────────
    size_t prev_dg_lp = 0; // links position of previous DG (for next_dg chaining)
    bool   first_dg   = true;

    for(const auto& g : groups){
        if(g.channels.empty() || g.time_axis_s.empty()) continue;
        uint64_t N = g.time_axis_s.size();

        // Pre-write TXBLOCKs for names/units
        size_t tx_cg = b.txblock(g.name);  // CG acquisition name

        // Time channel name/unit
        size_t tx_time_name = b.txblock(g.name + ".time_s");
        size_t tx_time_unit = b.txblock("s");

        std::vector<size_t> tx_ch_name(g.channels.size());
        std::vector<size_t> tx_ch_unit(g.channels.size());
        for(size_t i=0;i<g.channels.size();++i){
            tx_ch_name[i] = b.txblock(g.channels[i].name);
            tx_ch_unit[i] = b.txblock(g.channels[i].unit);
        }

        Layout lay = make_layout(g);

        // ── DGBLOCK (64 bytes) ────────────────────────────────────────────────
        size_t dg_pos = b.pos();
        size_t dg_lp  = b.blk_hdr("##DG", 64, 4);
        // links: [next_dg, cg, data=DT, comment=0] — ASAM link 3=data, link 4=comment
        b.u8(0); b.zeros(7); // rec_id_size=0, reserved

        if(first_dg){
            b.patch64(hd_lp+0, dg_pos); // hd_link[0] = first_dg
            first_dg = false;
        } else {
            b.patch64(prev_dg_lp+0, dg_pos); // prev_dg.next_dg = this_dg
        }
        prev_dg_lp = dg_lp;

        // ── CGBLOCK (104 bytes) ───────────────────────────────────────────────
        size_t cg_pos = b.pos();
        size_t cg_lp  = b.blk_hdr("##CG", 104, 6);
        // links: [next_cg=0, first_cn=0, acq_name, 0, 0, 0]
        b.patch64(cg_lp+16, tx_cg);     // cg_link[2] = acq_name
        b.patch64(dg_lp+8, cg_pos);     // dg_link[1] = cg

        // CG data (32 bytes):
        b.u64(0);              // cg_record_id
        b.u64(N);              // cg_cycle_count
        b.u16(0);              // cg_flags
        b.u16(0);              // cg_path_separator
        b.zeros(4);            // reserved
        b.u32(lay.rec_size);   // cg_data_bytes
        b.u32(0);              // cg_inval_bytes

        // ── CNBLOCKs (160 bytes each) ─────────────────────────────────────────
        // Time master channel first
        size_t prev_cn_lp = 0;

        auto write_cn = [&](size_t tx_name, size_t tx_unit,
                            uint8_t cn_type,     // 0=fixed, 2=master
                            uint8_t cn_sync,     // 0=none, 1=time
                            uint8_t cn_data_type,
                            uint32_t byte_off,
                            uint32_t bit_cnt)
        {
            size_t cn_pos = b.pos();
            size_t cn_lp  = b.blk_hdr("##CN", 160, 8);
            // links: [next_cn=0, comp=0, name, si=0, cc=0, data=0, unit, comment=0]
            b.patch64(cn_lp+16, tx_name);
            b.patch64(cn_lp+48, tx_unit);

            if(prev_cn_lp != 0)
                b.patch64(prev_cn_lp+0, cn_pos); // prev_cn.next_cn = this_cn
            else
                b.patch64(cg_lp+8, cn_pos);      // cg_link[1] = first_cn
            prev_cn_lp = cn_lp;

            // CN data (72 bytes):
            b.u8(cn_type);          // cn_type
            b.u8(cn_sync);          // cn_sync_type
            b.u8(cn_data_type);     // cn_data_type
            b.u8(0);                // cn_bit_offset
            b.u32(byte_off);        // cn_byte_offset
            b.u32(bit_cnt);         // cn_bit_count
            b.u32(0);               // cn_flags
            b.u32(0);               // cn_inval_bit_pos
            b.u8(0);                // cn_precision
            b.u8(0);                // cn_reserved
            b.u16(0);               // cn_attachment_count
            b.f64(0.0);             // val_range_min
            b.f64(0.0);             // val_range_max
            b.f64(0.0);             // limit_min
            b.f64(0.0);             // limit_max
            b.f64(0.0);             // limit_ext_min
            b.f64(0.0);             // limit_ext_max
        };

        // time master
        write_cn(tx_time_name, tx_time_unit, 2/*MASTER*/, 1/*TIME*/,
                 6/*double_le*/, 0/*byte_offset*/, 64/*bits*/);

        for(size_t i=0;i<g.channels.size();++i){
            auto ct = ctype(g.channels[i].data_type);
            write_cn(tx_ch_name[i], tx_ch_unit[i],
                     0/*FIXED*/, 0/*NONE*/,
                     ct.cn_data_type, lay.offsets[i+1], ct.bit_count);
        }

        // ── DTBLOCK ───────────────────────────────────────────────────────────
        size_t dt_pos  = b.pos();
        uint64_t dt_len = 24 + (uint64_t)N * lay.rec_size;
        b.blk_hdr("##DT", dt_len, 0);
        b.patch64(dg_lp+16, dt_pos); // dg_link[2] = data block (not comment link!)

        // Write interleaved records
        for(size_t row=0; row<(size_t)N; ++row){
            // Time channel (float64)
            b.f64(g.time_axis_s[row]);

            // Data channels
            for(size_t ci=0; ci<g.channels.size(); ++ci){
                const auto& ch  = g.channels[ci];
                double val = (row < ch.data.size()) ? ch.data[row]
                                                    : std::numeric_limits<double>::quiet_NaN();
                auto ct = ctype(ch.data_type);
                switch(ct.cn_data_type){
                    case 6: // float64
                        b.f64(val);
                        break;
                    case 4: // float32
                        b.f32(static_cast<float>(val));
                        break;
                    case 2: // int16
                        b.i16(static_cast<int16_t>(val));
                        break;
                    default: // uint8
                        b.u8(static_cast<uint8_t>(val));
                        break;
                }
            }
        }
    } // for each group

    return b.d;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

bool write_mdf4(
    const std::filesystem::path&     out_path,
    const std::vector<ChannelGroup>& groups,
    const load::SessionBundle&       meta,
    ns_t                             t0_ns)
{
    std::filesystem::create_directories(out_path.parent_path());

    auto tmp = out_path;
    tmp += ".tmp";

    try {
        auto bytes = build_mdf4(groups, meta, t0_ns);

        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if(!f) return false;
            f.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
            if(!f) return false;
        }

        std::filesystem::rename(tmp, out_path); // PI-04: atomic rename
        return true;
    } catch(...) {
        std::filesystem::remove(tmp);
        return false;
    }
}

bool validate_mdf4(
    const std::filesystem::path&     mdf_path,
    const std::vector<ChannelGroup>& groups)
{
    if(!std::filesystem::exists(mdf_path)) return false;

    // Verify MDF4 magic bytes ("MDF     " at offset 0)
    std::ifstream f(mdf_path, std::ios::binary);
    if(!f) return false;
    char magic[8] = {};
    f.read(magic, 8);
    if(std::strncmp(magic, "MDF     ", 8) != 0) return false;

    // Basic size sanity: file must be > 64 bytes header
    auto fsz = std::filesystem::file_size(mdf_path);
    if(fsz < 128) return false;

    // Check that expected sample counts are non-zero for non-empty groups
    for(const auto& g : groups){
        if(!g.channels.empty() && g.time_axis_s.empty())
            return false;
    }

    return true;
}

bool write_sha256_sidecar(const std::filesystem::path& mdf_path) {
    auto hex = sha256_file(mdf_path);
    if(hex.empty()) return false;

    auto sidecar = mdf_path;
    sidecar += ".sha256";
    std::ofstream f(sidecar);
    if(!f) return false;
    f << hex << "  " << mdf_path.filename().string() << '\n';
    return f.good();
}

} // namespace ego_offline::mdf4
