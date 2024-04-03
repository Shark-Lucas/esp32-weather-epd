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

    // 因为父类是模板类所以此处要定义需要使用的父类的成员
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
    using Adafruit_GFX::_height;

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

    // 重写以下三个方法，保证charBounds的调用链完整被重写，防止其他重载的父类接口被调用
    void getTextBounds(const char *str, int16_t x, int16_t y, int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h)
    {
        uint8_t c; // Current character
        int16_t minx = 0x7FFF, miny = 0x7FFF, maxx = -1, maxy = -1; // Bound rect
        // Bound rect is intentionally initialized inverted, so 1st char sets it

        *x1 = x; // Initial position is value passed in
        *y1 = y;
        *w = *h = 0; // Initial size is zero

        while ((c = *str++)) {
            // charBounds() modifies x/y to advance for each character,
            // and min/max x/y are updated to incrementally build bounding rect.
            charBounds(c, &x, &y, &minx, &miny, &maxx, &maxy);
        }

        if (maxx >= minx) {     // If legit string bounds were found...
            *x1 = minx;           // Update x1 to least X coord,
            *w = maxx - minx + 1; // And w to bound rect width
        }
        if (maxy >= miny) { // Same for height
            *y1 = miny;
            *h = maxy - miny + 1;
        }
    }

    void getTextBounds(const __FlashStringHelper *str, int16_t x, int16_t y, int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h)
    {
        uint8_t *s = (uint8_t *)str, c;

        *x1 = x;
        *y1 = y;
        *w = *h = 0;

        int16_t minx = _width, miny = _height, maxx = -1, maxy = -1;

        while ((c = pgm_read_byte(s++)))
        {
            charBounds(c, &x, &y, &minx, &miny, &maxx, &maxy);
        }

        if (maxx >= minx) {
            *x1 = minx;
            *w = maxx - minx + 1;
        }
        if (maxy >= miny) {
            *y1 = miny;
            *h = maxy - miny + 1;
        }
    }

    void getTextBounds(const String &str, int16_t x, int16_t y, int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h)
    {
        if (str.length() != 0)
        {
            getTextBounds(const_cast<char *>(str.c_str()), x, y, x1, y1, w, h);
        }
    }


protected:
    uint8_t writecount = 0;
    uint8_t temparray[3];

    // 重写该方法，兼容 utf-8 编码的汉字的宽度获取
    void charBounds(unsigned char c, int16_t *x, int16_t *y, int16_t *minx, int16_t *miny, int16_t *maxx, int16_t *maxy)
    {
        uint16_t unicode;
        uint16_t ch_count = gfxFont->ch_count;
        uint16_t *dat = gfxFont->Chinese_;
        uint8_t first = pgm_read_byte(&gfxFont->first);

        if (c == '\n')
        {
            *x = 0;        // Reset x to zero, advance y by one line
            *y += textsize_y * (uint8_t)pgm_read_byte(&gfxFont->yAdvance);
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
                        
                        uint8_t gw = pgm_read_byte(&glyph->width),
                                gh = pgm_read_byte(&glyph->height),
                                xa = pgm_read_byte(&glyph->xAdvance);

                        int8_t xo = pgm_read_byte(&glyph->xOffset),
                               yo = pgm_read_byte(&glyph->yOffset);
                        if (wrap && ((*x + (((int16_t)xo + gw) * textsize_x)) > _width))
                        {
                            *x = 0; // Reset x to zero, advance y by one line
                            *y += textsize_y * (uint8_t)pgm_read_byte(&gfxFont->yAdvance);
                        }
                        int16_t tsx = (int16_t)textsize_x, 
                                tsy = (int16_t)textsize_y,
                                x1 = *x + xo * tsx, 
                                y1 = *y + yo * tsy, 
                                x2 = x1 + gw * tsx - 1,
                                y2 = y1 + gh * tsy - 1;

                        if (x1 < *minx)
                            *minx = x1;
                        if (y1 < *miny)
                            *miny = y1;
                        if (x2 > *maxx)
                            *maxx = x2;
                        if (y2 > *maxy)
                            *maxy = y2;

                        *x += xa * tsx;
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

                        uint8_t gw = pgm_read_byte(&glyph->width),
                                gh = pgm_read_byte(&glyph->height),
                                xa = pgm_read_byte(&glyph->xAdvance);
                        int8_t xo = pgm_read_byte(&glyph->xOffset),
                               yo = pgm_read_byte(&glyph->yOffset);
                        if (wrap && ((*x + (((int16_t)xo + gw) * textsize_x)) > _width))
                        {
                            *x = 0; // Reset x to zero, advance y by one line
                            *y += textsize_y * (uint8_t)pgm_read_byte(&gfxFont->yAdvance);
                        }
                        int16_t tsx = (int16_t)textsize_x, 
                                tsy = (int16_t)textsize_y,
                                x1 = *x + xo * tsx, 
                                y1 = *y + yo * tsy, 
                                x2 = x1 + gw * tsx - 1,
                                y2 = y1 + gh * tsy - 1;

                        if (x1 < *minx)
                            *minx = x1;
                        if (y1 < *miny)
                            *miny = y1;
                        if (x2 > *maxx)
                            *maxx = x2;
                        if (y2 > *maxy)
                            *maxy = y2;
                            
                        *x += xa * tsx;
                    }
                }
            }
        }
    }
};


#endif // MYGXEPD2_BW_H
