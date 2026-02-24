/*
 * BNTX to DDS Converter
 * Extracts all textures from Nintendo Switch BNTX files and saves as DDS/PNG
 * Copyright © Arulo 2026
 * Based on BNTX Extractor v0.6 by AboodXD
 * 
 * Output: Creates .dds files for each texture
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <map>
#include <algorithm>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;


// ============================================================================
// DATA TABLES 
// ============================================================================

std::map<u32, std::string> formats = {
    {0x0b, "R8_G8_B8_A8"}, {0x07, "R5_G6_B5"}, {0x02, "R8_UNORM"}, {0x09, "R8_G8"},
    {0x1a, "BC1"}, {0x1b, "BC2"}, {0x1c, "BC3"}, {0x1d, "BC4"}, {0x1e, "BC5"},
    {0x1f, "BC6H"}, {0x20, "BC7"},
    {0x2d, "ASTC4x4"}, {0x2e, "ASTC5x4"}, {0x2f, "ASTC5x5"}, {0x30, "ASTC6x5"},
    {0x31, "ASTC6x6"}, {0x32, "ASTC8x5"}, {0x33, "ASTC8x6"}, {0x34, "ASTC8x8"},
    {0x35, "ASTC10x5"}, {0x36, "ASTC10x6"}, {0x37, "ASTC10x8"}, {0x38, "ASTC10x10"},
    {0x39, "ASTC12x10"}, {0x3a, "ASTC12x12"}
};

std::map<u32, u32> bpps = {
    {0x0b, 4}, {0x07, 2}, {0x02, 1}, {0x09, 2}, {0x1a, 8},
    {0x1b, 16}, {0x1c, 16}, {0x1d, 8}, {0x1e, 16}, {0x1f, 16},
    {0x20, 16}, {0x2d, 16}, {0x2e, 16}, {0x2f, 16}, {0x30, 16},
    {0x31, 16}, {0x32, 16}, {0x33, 16}, {0x34, 16}, {0x35, 16},
    {0x36, 16}, {0x37, 16}, {0x38, 16}, {0x39, 16}, {0x3a, 16}
};

struct BlockDim { 
    u32 first;  
    u32 second; 
};
std::map<u32, BlockDim> blkDims = {
    {0x1a, {4, 4}}, {0x1b, {4, 4}}, {0x1c, {4, 4}}, {0x1d, {4, 4}}, {0x1e, {4, 4}},
    {0x1f, {4, 4}}, {0x20, {4, 4}}, {0x2d, {4, 4}}, {0x2e, {5, 4}}, {0x2f, {5, 5}},
    {0x30, {6, 5}}, {0x31, {6, 6}}, {0x32, {8, 5}}, {0x33, {8, 6}}, {0x34, {8, 8}},
    {0x35, {10, 5}}, {0x36, {10, 6}}, {0x37, {10, 8}}, {0x38, {10, 10}}, {0x39, {12, 10}},
    {0x3a, {12, 12}}
};

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

inline u32 Read32LE(const u8* data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

inline u16 Read16LE(const u8* data) {
    return data[0] | (data[1] << 8);
}

inline u64 Read64LE(const u8* data) {
    u64 result = 0;
    for (int i = 0; i < 8; i++) {
        result |= ((u64)data[i]) << (i * 8);
    }
    return result;
}

inline i64 Read64LE_Signed(const u8* data) {
    return (i64)Read64LE(data);
}

std::string ReadString(const u8* data, size_t maxLen) {
    std::string result;
    for (size_t i = 0; i < maxLen && data[i] != 0; i++) {
        result += (char)data[i];
    }
    return result;
}

// ============================================================================
// TEGRA BLOCK LINEAR SWIZZLE
// ============================================================================

inline u32 DIV_ROUND_UP(u32 n, u32 d) {
    return (n + d - 1) / d;
}

inline u32 round_up(u32 x, u32 y) {
    return ((x - 1) | (y - 1)) + 1;
}

u32 getAddrBlockLinear(u32 x, u32 y, u32 image_width, u32 bytes_per_pixel, 
                       u32 base_address, u32 block_height) {
    u32 image_width_in_gobs = DIV_ROUND_UP(image_width * bytes_per_pixel, 64);
    
    u32 GOB_address = (base_address
                       + (y / (8 * block_height)) * 512 * block_height * image_width_in_gobs
                       + (x * bytes_per_pixel / 64) * 512 * block_height
                       + (y % (8 * block_height) / 8) * 512);
    
    x *= bytes_per_pixel;
    
    u32 Address = (GOB_address + ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64
                   + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16));
    
    return Address;
}

std::vector<u8> deswizzle(u32 width, u32 height, u32 blkWidth, u32 blkHeight, 
                          u32 bpp, u32 tileMode, u32 alignment, u32 size_range, 
                          const std::vector<u8>& data) {
    
    u32 block_height = 1 << size_range;
    
    width = DIV_ROUND_UP(width, blkWidth);
    height = DIV_ROUND_UP(height, blkHeight);
    
    u32 pitch, surfSize;
    
    if (tileMode == 0) {
        pitch = round_up(width * bpp, 32);
        surfSize = round_up(pitch * height, alignment);
    } else {
        pitch = round_up(width * bpp, 64);
        surfSize = round_up(pitch * round_up(height, block_height * 8), alignment);
    }
    
    std::vector<u8> result(surfSize, 0);
    
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 pos;
            
            if (tileMode == 0) {
                pos = y * pitch + x * bpp;
            } else {
                pos = getAddrBlockLinear(x, y, width, bpp, 0, block_height);
            }
            
            u32 pos_ = (y * width + x) * bpp;
            
            if (pos + bpp <= surfSize && pos_ + bpp <= data.size()) {
                std::memcpy(&result[pos_], &data[pos], bpp);
            }
        }
    }
    
    return result;
}

// ============================================================================
// DDS HEADER GENERATION
// ============================================================================

std::vector<u8> generateDDSHeader(u32 width, u32 height, u32 format, u32 size) {
    std::vector<u8> header(128, 0);
    
    // DDS magic
    header[0] = 'D'; header[1] = 'D'; header[2] = 'S'; header[3] = ' ';
    
    // Header size
    u32 headerSize = 124;
    std::memcpy(&header[4], &headerSize, 4);
    
    // Flags: CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE
    u32 flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;
    std::memcpy(&header[8], &flags, 4);
    
    // Height & Width
    std::memcpy(&header[12], &height, 4);
    std::memcpy(&header[16], &width, 4);
    
    // Pitch/LinearSize
    std::memcpy(&header[20], &size, 4);
    
    // Mipmap count
    u32 mips = 1;
    std::memcpy(&header[28], &mips, 4);
    
    // Pixel format (dwSize = 32)
    u32 pfSize = 32;
    std::memcpy(&header[76], &pfSize, 4);
    
    // Pixel format flags (FOURCC)
    u32 pfFlags = 0x4;
    std::memcpy(&header[80], &pfFlags, 4);
    
    // FourCC based on format
    const char* fourcc = nullptr;
    
    if (format == 0x1a) fourcc = "DXT1";
    else if (format == 0x1b) fourcc = "DXT3";
    else if (format == 0x1c) fourcc = "DXT5";
    else if (format == 0x1d) fourcc = "ATI1"; // BC4
    else if (format == 0x1e) fourcc = "ATI2"; // BC5
    else if (format == 0x1f) fourcc = "BC6H";
    else if (format == 0x20) fourcc = "BC7 ";
    
    if (fourcc) {
        std::memcpy(&header[84], fourcc, 4);
    }
    
    // Caps
    u32 caps1 = 0x1000; // TEXTURE
    std::memcpy(&header[108], &caps1, 4);
    
    return header;
}

// ============================================================================
// BNTX STRUCTURES
// ============================================================================

struct BNTXTexture {
    std::string name;
    u32 width;
    u32 height;
    u32 format;
    u32 tileMode;
    u32 sizeRange;
    u32 alignment;
    u32 imageSize;
    std::vector<u8> data;
};

// ============================================================================
// BNTX PARSER
// ============================================================================

std::vector<BNTXTexture> parseBNTX(const std::vector<u8>& f) {
    std::vector<BNTXTexture> textures;
    
    if (f.size() < 0x100) {
        std::cerr << "File too small!" << std::endl;
        return textures;
    }
   
    if (std::memcmp(&f[0], "BNTX", 4) != 0) {
        std::cerr << "Not a valid BNTX file!" << std::endl;
        return textures;
    }
    
    bool littleEndian = (f[0xc] == 0xFF && f[0xd] == 0xFE);
    if (!littleEndian) {
        std::cerr << "Big endian not supported!" << std::endl;
        return textures;
    }
    
    std::cout << "BNTX file detected" << std::endl;
    
    u32 pos = 0;
    u32 fileNameAddr = Read32LE(&f[pos + 0x10]);
    u32 fileSize = Read32LE(&f[pos + 0x1C]);
    
    std::string fileName = ReadString(&f[fileNameAddr], 256);
    std::cout << "File name: " << fileName << std::endl;
    std::cout << "File size: " << fileSize << std::endl;
    
    pos += 0x20; 
    
    if (std::memcmp(&f[pos], "NX  ", 4) != 0) {
        std::cerr << "Invalid NX header!" << std::endl;
        return textures;
    }
    
    u32 texCount = Read32LE(&f[pos + 0x04]);
    i64 infoPtrAddr = Read64LE_Signed(&f[pos + 0x08]);
    i64 dataBlkAddr = Read64LE_Signed(&f[pos + 0x10]);
    
    std::cout << "Textures count: " << texCount << std::endl;
    
    for (u32 i = 0; i < texCount; i++) {
        i64 infoPtr = infoPtrAddr + i * 8;
        i64 texInfoAddr = Read64LE_Signed(&f[infoPtr]);
        
        if (texInfoAddr < 0 || texInfoAddr >= (i64)f.size()) {
            std::cerr << "Invalid texture info address!" << std::endl;
            continue;
        }
        
        pos = texInfoAddr;
        
        if (std::memcmp(&f[pos], "BRTI", 4) != 0) {
            std::cerr << "Invalid BRTI magic!" << std::endl;
            continue;
        }
        
        u8 tileMode = f[pos + 0x10];
        u16 flags = Read16LE(&f[pos + 0x12]);
        u16 swizzle = Read16LE(&f[pos + 0x14]);
        u16 numMips = Read16LE(&f[pos + 0x16]);
        u32 format = Read32LE(&f[pos + 0x1C]);
        u32 width = Read32LE(&f[pos + 0x24]);
        u32 height = Read32LE(&f[pos + 0x28]);
        u32 sizeRange = Read32LE(&f[pos + 0x34]);
        u32 imageSize = Read32LE(&f[pos + 0x50]);
        u32 alignment = Read32LE(&f[pos + 0x54]);
        i64 nameAddr = Read64LE_Signed(&f[pos + 0x60]);
        i64 ptrsAddr = Read64LE_Signed(&f[pos + 0x70]);
        
        u16 nameLen = Read16LE(&f[nameAddr]);
        std::string name = ReadString(&f[nameAddr + 2], nameLen);
        
        std::cout << "\n=== Image " << (i+1) << " ===" << std::endl;
        std::cout << "Name: " << name << std::endl;
        std::cout << "Width: " << width << std::endl;
        std::cout << "Height: " << height << std::endl;
        
        if (formats.find(format) != formats.end()) {
            std::cout << "Format: " << formats[format] << std::endl;
        } else {
            std::cout << "Format: 0x" << std::hex << format << std::dec << std::endl;
        }
        
        std::cout << "TileMode: " << (tileMode == 0 ? "LINEAR" : "BLOCK_LINEAR") << std::endl;
        std::cout << "Block Height: " << (1 << sizeRange) << std::endl;
        std::cout << "Image Size: " << imageSize << std::endl;
        
        i64 dataAddr = Read64LE_Signed(&f[ptrsAddr]);
        
        if (dataAddr < 0 || dataAddr + imageSize > f.size()) {
            std::cerr << "Invalid data address!" << std::endl;
            continue;
        }
        
        BNTXTexture tex;
        tex.name = name;
        tex.width = width;
        tex.height = height;
        tex.format = format;
        tex.tileMode = tileMode;
        tex.sizeRange = sizeRange;
        tex.alignment = alignment;
        tex.imageSize = imageSize;
        tex.data.resize(imageSize);
        std::memcpy(tex.data.data(), &f[dataAddr], imageSize);
        
        textures.push_back(tex);
    }
    
    return textures;
}

// ============================================================================
// TEXTURE EXPORT
// ============================================================================

void saveTextures(const std::vector<BNTXTexture>& textures, const std::string& outputDir) {
    for (const auto& tex : textures) {
        u32 formatType = tex.format >> 8;
        
        if (formats.find(formatType) == formats.end()) {
            std::cout << "\nSkipping " << tex.name << " - unsupported format (0x" 
                      << std::hex << tex.format << std::dec << ")" << std::endl;
            continue;
        }
        
        u32 blkWidth = 1, blkHeight = 1, bpp = 4;
        
        if (blkDims.find(formatType) != blkDims.end()) {
            blkWidth = blkDims[formatType].first;
            blkHeight = blkDims[formatType].second;
        }
        
        if (bpps.find(formatType) != bpps.end()) {
            bpp = bpps[formatType];
        }
        
        u32 size = DIV_ROUND_UP(tex.width, blkWidth) * DIV_ROUND_UP(tex.height, blkHeight) * bpp;
        
        std::cout << "\nProcessing: " << tex.name << " (" << formats[formatType] << ")" << std::endl;
        
        std::vector<u8> result = deswizzle(
            tex.width, tex.height, 
            blkWidth, blkHeight, 
            bpp, tex.tileMode, 
            tex.alignment, tex.sizeRange, 
            tex.data
        );
        
        if (result.size() > size) {
            result.resize(size);
        }
        
        std::vector<u8> header = generateDDSHeader(tex.width, tex.height, formatType, size);
        
        std::string outName = outputDir + "/" + tex.name + ".dds";
        std::ofstream out(outName, std::ios::binary);
        if (!out) {
            std::cerr << "Failed to create " << outName << std::endl;
            continue;
        }
        
        out.write((char*)header.data(), header.size());
        out.write((char*)result.data(), result.size());
        out.close();
        
        std::cout << "Saved: " << outName << std::endl;
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "BNTX to DDS Converter" << std::endl;
    std::cout << "==========================================\n" << std::endl;

    std::string inputPath;
    std::string outputDir;

    std::cout << "Path to .bntx file: ";
    std::getline(std::cin, inputPath);

    std::cout << "Output Path: ";
    std::getline(std::cin, outputDir);

    auto cleanPath = [](std::string& path) {
        if (!path.empty() && (path.front() == '"' || path.front() == '\'')) {
            path.erase(0, 1);
        }
        if (!path.empty() && (path.back() == '"' || path.back() == '\'')) {
            path.pop_back();
        }
    };
    cleanPath(inputPath);
    cleanPath(outputDir);

    if (inputPath.empty() || outputDir.empty()) {
        std::cerr << "Error: Path is empty" << std::endl;
        std::cout << "\nPress Enter to quit...";
        std::cin.get();
        return 1;
    }

    #ifdef _WIN32
        std::string mkdirCmd = "mkdir \"" + outputDir + "\" 2>nul";
    #else
        std::string mkdirCmd = "mkdir -p \"" + outputDir + "\"";
    #endif
    system(mkdirCmd.c_str());

    std::cout << "\nLese Datei: " << inputPath << "..." << std::endl;

    std::ifstream file(inputPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Error file couldnt be opened" << std::endl;
        std::cout << "\nPress Enter to quit...";
        std::cin.get();
        return 1;
    }

    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<u8> fileData(fileSize);
    if (!file.read(reinterpret_cast<char*>(fileData.data()), fileSize)) {
        std::cerr << "Error: File couldnt be read" << std::endl;
        return 1;
    }
    file.close();

    auto textures = parseBNTX(fileData);

    if (textures.empty()) {
        std::cerr << "Error: No textures found in file!" << std::endl;
        std::cout << "\nPress Enter to quit...";
        std::cin.get();
        return 1;
    }

    saveTextures(textures, outputDir);

    std::cout << "\n==========================================" << std::endl;
    std::cout << "Finished! " << textures.size() << " Textures extracted to '" 
              << outputDir<< std::endl;
    
    std::cout << "\nPress Enter to quit...";
    std::cin.get(); 

    return 0;
}
