#include <cstring>
#if defined(_WIN32) || defined(WIN32)
    #include <io.h>
    #define isatty(h) _isatty(h)
#else
    #include <unistd.h>
#endif

#include <fstream>
#include <iostream>

struct Pixel {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
};

struct QoiHeader {
  char magic[4];
  uint32_t width;
  uint32_t height;
  uint8_t channels;
  uint8_t colorspace;
};

// An owning class which allows you to interact with the image more easily
class Image {
  Pixel *a;
  uint32_t width;
  uint32_t height;
  Image &operator=(const Image &) = delete;
  Image(const Image &) = delete;

public:
  Image(uint32_t width, uint32_t height) {
    this->width = width;
    this->height = height;
    this->a = new Pixel[this->width * this->height];
  }

  ~Image() { delete[] this->a; }
  uint32_t get_width() { return width; }

  uint32_t get_height() { return height; }

  Pixel get(uint32_t row, uint32_t col) {
    return this->a[row * this->width + col];
  }

  void set(uint32_t row, uint32_t col, Pixel val) {
    this->a[row * this->width + col] = val;
  }
};

int qoi_pix_hash(const Pixel &p) {
  return ((int)(p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11)) % 64;
}

void do_decode_image(QoiHeader h, std::istream &in, Image &decoded) {
  // A running array[64] (zero-initialized) (QOI spec)
  Pixel memory[64];
  for (int i = 0; i < 64; i++)
    memory[i] = Pixel{0, 0, 0, 0};

  Pixel last_pix = Pixel{0, 0, 0, 255};

  uint32_t run_length = 0;
  for (uint32_t i = 0; i < h.height; i++) {
    for (uint32_t j = 0; j < h.width; j++) {
      if (run_length != 0) {
        decoded.set(i, j, last_pix);
        run_length--;
        continue;
      }

      uint8_t starting_byte;
      in.read((char *)&starting_byte, sizeof(starting_byte));

      Pixel res = Pixel{0, 0, 0, 255}; // The decoder and encoder start with {r:
                                       // 0, g: 0, b: 0, a: 255}   (QOI spec)

      if (starting_byte == 0b1111'1110) { // QOI_OP_RGB
        in.read((char *)&res.r, sizeof(res.r));
        in.read((char *)&res.g, sizeof(res.g));
        in.read((char *)&res.b, sizeof(res.b));
        // The alpha value remains unchanged form the previous pixel (QOI spec)
        // NOTE: This is important for hashing
        // NOTE: Originally i missed this part and it tripped me up, causing bad
        // decoding but only on some images with transparency, and only on some
        // parts, (ex. dice.qoi)
        res.a = last_pix.a;
        memory[qoi_pix_hash(res)] = res;
      } else if (starting_byte == 0b1111'1111) { // QOI_OP_RGBA
        in.read((char *)&res.r, sizeof(res.r));
        in.read((char *)&res.g, sizeof(res.g));
        in.read((char *)&res.b, sizeof(res.b));
        in.read((char *)&res.a, sizeof(res.a));
        memory[qoi_pix_hash(res)] = res;
      } else if (((starting_byte >> 6) & 0b11) == 0b00) { // QOI_OP_INDEX
        uint32_t index = starting_byte & 0b11'1111;
        res = memory[index];
        // NOTE: Since res came from memory, there is no need to save res to
        // memory
      } else if (((starting_byte >> 6) & 0b11) == 0b01) { // QOI_OP_DIFF
        // Bias of +2, so we need to -2 to get the correct value
        res.r = last_pix.r + ((starting_byte >> 4) & 0b11) - 2;
        res.g = last_pix.g + ((starting_byte >> 2) & 0b11) - 2;
        res.b = last_pix.b + ((starting_byte >> 0) & 0b11) - 2;
        res.a = last_pix.a;
        memory[qoi_pix_hash(res)] = res;
      } else if (((starting_byte >> 6) & 0b11) == 0b10) { // QOI_OP_LUMA
        uint8_t second_byte;
        in.read((char *)&second_byte, sizeof(second_byte));

        // Values are stored as unsigned integers with a bias of 32 for the
        // green channel and a bias of 8 for the red and blue channel.
        // (QOI spec)
        uint8_t dg = (uint8_t)(starting_byte & 0b11'1111) - 32;
        uint8_t dr_minus_dg = (uint8_t)((second_byte >> 4) & 0b1111) - 8;
        uint8_t db_minus_dg = (uint8_t)((second_byte >> 0) & 0b1111) - 8;

        // The difference to the current channel values use a wraparound
        // operation (QOI spec)
        res.r = last_pix.r + dr_minus_dg + dg;
        res.g = last_pix.g + dg;
        res.b = last_pix.b + db_minus_dg + dg;
        // The alpha value remains unchanged from the previous pixel. (QOI spec)
        res.a = last_pix.a;
        memory[qoi_pix_hash(res)] = res;
      } else if (((starting_byte >> 6) & 0b11) == 0b11) { // QOI_OP_RUN
        // Bias of -1, so we need to +1 to get the correct value
        run_length = (starting_byte & 0b11'1111) + 1;
        res = last_pix;
        run_length--;
      } else {
        std::cerr << "Unknown chunk with starting byte: " << starting_byte
                  << ", at pixel x: " << j << ", y: " << i << std::endl;
        exit(-1);
      }

      decoded.set(i, j, res);
      last_pix = res;
    }
  }
}

uint32_t swap_bytes(uint32_t input) {
  return ((input >> 24) & 0xff) << 0 | ((input >> 16) & 0xff) << 8 |
         ((input >> 8) & 0xff) << 16 | ((input >> 0) & 0xff) << 24;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " [-f] (file.qoi) " << std::endl;
    return -1;
  }

  if (isatty(fileno(stdout)) && memcmp(argv[1], "-f", 2) != 0) {
    std::cerr << "Refusing to output .pnm to terminal, pass -f to override!"
              << std::endl;
    return -2;
  }

  std::ifstream in(argv[argc - 1], std::ios::binary);
  QoiHeader header;

  // Read header
  in.read((char *)header.magic, sizeof(char) * 4);
  in.read((char *)&header.width, sizeof(header.width));
  header.width = swap_bytes(header.width);
  in.read((char *)&header.height, sizeof(header.height));
  header.height = swap_bytes(header.height);
  in.read((char *)&header.channels, sizeof(header.channels));
  in.read((char *)&header.colorspace, sizeof(header.colorspace));

  if (memcmp(header.magic, "qoif", 4) == 0) {
    Image decoded(header.width, header.height);
    do_decode_image(header, in, decoded);
    std::cout << "P6" << std::endl;
    std::cout << header.width << " " << header.height << std::endl;
    std::cout << "255" << std::endl;
    for (uint32_t i = 0; i < header.height; i++) {
      for (uint32_t j = 0; j < header.width; j++) {
        Pixel p = decoded.get(i, j);
        std::cout.write((char *)&(p.r), sizeof(p.r));
        std::cout.write((char *)&(p.g), sizeof(p.g));
        std::cout.write((char *)&(p.b), sizeof(p.b));
      }
    }
  } else {
    std::cerr << "Bad qoi header (incorrect magic)!" << std::endl;
    return -1;
  }
}