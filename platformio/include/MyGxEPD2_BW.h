#ifndef MYGXEPD2_BW_H
#define MYGXEPD2_BW_H

#include <vector>
#include <Arduino.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <Adafruit_GFX.h>

template <typename GxEPD2_Type, const uint16_t page_height>
class MyGxEPD2_BW : public GxEPD2_BW<GxEPD2_Type, page_height>
{
public:
    MyGxEPD2_BW(GxEPD2_Type epd2_instance) : GxEPD2_BW<GxEPD2_Type, page_height>(epd2_instance)
    {
        // 子类构造函数实现，直接调用父类构造函数
    }

    using Adafruit_GFX::drawChar;
    using Adafruit_GFX::gfxFont;
    using Adafruit_GFX::cursor_x;
    using Adafruit_GFX::cursor_y;
    using Adafruit_GFX::textsize_x;
    using Adafruit_GFX::textsize_y;
    using Adafruit_GFX::wrap;
    using Adafruit_GFX::textcolor;
    using Adafruit_GFX::textbgcolor;
    using Adafruit_GFX::_width;

    GFXglyph *pgm_read_glyph_ptr(const GFXfont *gfxFont, uint8_t c)
    {
        return gfxFont->glyph + c;
    }

    uint16_t utf8ToUnicode(const uint8_t *utf8Char)
    {
        // 检查输入指针
        if (!utf8Char) {
            return 0;
        }
        Serial.printf("utf8: 0x%02x%02x%02x\n", utf8Char[0], utf8Char[1], utf8Char[2]);
        // UTF-8汉字总是占用三个字节
        // 确保这是三字节字符
        if ((utf8Char[0] & 0xF0) == 0xE0) {
            // 将三个字节转换为一个Unicode字符
            return ((utf8Char[0] & 0x0F) << 12) | ((utf8Char[1] & 0x3F) << 6) | (utf8Char[2] & 0x3F);
        }

        // 如果不是三字节字符，则返回0
        return 0;
    }

    // 重写的 write 实现，用于多字节字符集 utf-8 中文的处理，转换为UNICODE
    size_t write(uint8_t c) override
    {
        uint16_t unicode;
        uint16_t ch_count = gfxFont->ch_count;
        uint16_t *dat = gfxFont->Chinese_;
        uint8_t first = pgm_read_byte(&gfxFont->first);

        if (c == '\n')
        {
            cursor_x = 0;
            cursor_y += (int16_t)textsize_y * (uint8_t)pgm_read_byte(&gfxFont->yAdvance);
        }
        else if (c != '\r')
        {
            if (writecount != 0)
            {
                temparray[writecount] = c;
                writecount++;
                if (writecount == 3)
                {
                    writecount = 0;
                    // utf-8 to unicode
                    // unicode = ((uint8_t(int(temparray[0]) << 4)) >> 4);
                    // unicode = unicode << 6 | ((uint8_t(int(temparray[1]) << 2)) >> 2);
                    // unicode = unicode << 6 | ((uint8_t(int(temparray[2]) << 2)) >> 2);
                    unicode = utf8ToUnicode(temparray);

                    char fmt[22];
                    sprintf(fmt, "0x%04X", unicode);
                    Serial.println("unicode: " + String(fmt));
                    // 查找 C 并确认 C 的下标
                    while (ch_count--)
                    {
                        if (dat[ch_count] == unicode)
                        {
                            c = 127 + ch_count;
                            ch_count = 0;
                        }
                        else
                        {
                            c = 42;
                        }
                    }

                    // 判断 C 是否在范围内
                    if ((c >= first) && (c <= (uint8_t)pgm_read_byte(&gfxFont->last)))
                    {
                        GFXglyph *glyph = pgm_read_glyph_ptr(gfxFont, c - first);
                        uint8_t w = pgm_read_byte(&glyph->width),
                                h = pgm_read_byte(&glyph->height);
                        if ((w > 0) && (h > 0))
                        {                                                        // Is there an associated bitmap?
                            int16_t xo = (int8_t)pgm_read_byte(&glyph->xOffset); // sic
                            if (wrap && ((cursor_x + textsize_x * (xo + w)) > _width))
                            {
                                cursor_x = 0;
                                cursor_y += (int16_t)textsize_y * (uint8_t)pgm_read_byte(&gfxFont->yAdvance);
                            }
                            drawChar(cursor_x, cursor_y, c, textcolor, textbgcolor, textsize_x, textsize_y);
                        }
                        cursor_x += (uint8_t)pgm_read_byte(&glyph->xAdvance) * (int16_t)textsize_x;
                    }
                }
            }
            else
            {
                if ((c > 127) && ( c != 0xB0))
                {
                    // 非ascii码，是 utf-8
                    temparray[writecount] = c;
                    writecount++;
                }
                else
                {
                    // 是 ascii 码，正常处理
                    if ((c >= first) && (c <= (uint8_t)pgm_read_byte(&gfxFont->last)))
                    {
                        if (c == 0xB0) c = 0x7f;
                        GFXglyph *glyph = pgm_read_glyph_ptr(gfxFont, c - first);
                        uint8_t w = pgm_read_byte(&glyph->width),
                                h = pgm_read_byte(&glyph->height);
                        if ((w > 0) && (h > 0))
                        {                                                        // Is there an associated bitmap?
                            int16_t xo = (int8_t)pgm_read_byte(&glyph->xOffset); // sic
                            if (wrap && ((cursor_x + textsize_x * (xo + w)) > _width))
                            {
                                cursor_x = 0;
                                cursor_y += (int16_t)textsize_y * (uint8_t)pgm_read_byte(&gfxFont->yAdvance);
                            }
                            drawChar(cursor_x, cursor_y, c, textcolor, textbgcolor, textsize_x, textsize_y);
                        }
                        cursor_x += (uint8_t)pgm_read_byte(&glyph->xAdvance) * (int16_t)textsize_x;
                    }
                }
            }
        }
        return 1;
    }


protected:
    uint8_t writecount = 0;
    uint8_t temparray[3];

};


#endif // MYGXEPD2_BW_H
