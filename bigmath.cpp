#include "bigmath.h"
#include <iostream>
#include <cstdio>

BigUnsigned::Data * BigUnsigned::smallNumbers = NULL;

static inline void addWithCarry(WordType a, WordType b, bool carryIn, WordType & result, bool & carryOut)
{
    DoubleWordType v = a;
    v += b;
    v += carryIn ? 1 : 0;
    carryOut = (v > WordMax);
    result = (WordType)(v & WordMax);
}

static inline void subtractWithBorrow(WordType a, WordType b, bool borrowIn, WordType & result, bool & borrowOut)
{
    DoubleWordType v = a;
    v |= (DoubleWordType)WordMax + 1;
    v -= b;
    v -= borrowIn ? 1 : 0;
    borrowOut = (v <= WordMax);
    result = (WordType)(v & WordMax);
}

static inline void multiplyDoubleWord(WordType a, WordType b, WordType & highWord, WordType & lowWord)
{
    DoubleWordType v = a;
    v *= b;
    highWord = (WordType)(v >> BitsPerWord);
    lowWord = (WordType)(v & WordMax);
}

static inline void multiplyDoubleWordAndAdd(WordType a, WordType b, WordType term, WordType & highWord, WordType & lowWord)
{
    DoubleWordType v = a;
    v *= b;
    v += term;
    highWord = (WordType)(v >> BitsPerWord);
    lowWord = (WordType)(v & WordMax);
}

static inline void multiplyDoubleWordAndAddTwo(WordType a, WordType b, WordType term1, WordType term2, WordType & highWord, WordType & lowWord)
{
    DoubleWordType v = a;
    v *= b;
    v += term1;
    v += term2;
    highWord = (WordType)(v >> BitsPerWord);
    lowWord = (WordType)(v & WordMax);
}

static inline void divideDoubleWord(WordType dividendHighWord, WordType dividendLowWord, WordType divisor, WordType & quotient, WordType & remainder)
{
    DoubleWordType dividend = dividendHighWord;
    dividend <<= BitsPerWord;
    dividend |= dividendLowWord;
    quotient = (WordType)(dividend / divisor);
    remainder = (WordType)(dividend % divisor);
}

static inline void lshiftDoubleWord(WordType highWordIn, WordType lowWordIn, size_t shiftCount, WordType & highWordOut)
{
    DoubleWordType v = highWordIn;
    v <<= BitsPerWord;
    v |= lowWordIn;
    v <<= shiftCount;
    highWordOut = (WordType)(v >> BitsPerWord);
}

static inline void rshiftDoubleWord(WordType highWordIn, WordType lowWordIn, size_t shiftCount, WordType & lowWordOut)
{
    DoubleWordType v = highWordIn;
    v <<= BitsPerWord;
    v |= lowWordIn;
    v >>= shiftCount;
    lowWordOut = (WordType)(v & WordMax);
}

static void handleError(string msg)
{
#if 0
    throw runtime_error(msg);
#else
    cerr << "Error : " << msg << endl;
    exit(1);
#endif
}

BigUnsigned BigUnsigned::parseHexByteString(string str)
{
    size_t byteCount = 0;
    int hexDigitCount = 0;
    for(size_t i = 0; i < str.size(); i++)
    {
        if(isxdigit(str[i]))
        {
            if(hexDigitCount == 0)
            {
                byteCount++;
            }
            hexDigitCount++;
            if(hexDigitCount > 2)
            {
                handleError("too many digits for a byte in BigUnsigned::parseHexByteString");
            }
        }
        else if(str[i] == ':')
        {
            if(hexDigitCount == 0)
                handleError("no digits for a byte in BigUnsigned::parseHexByteString");
            hexDigitCount = 0;
        }
        else
            handleError("invalid character in BigUnsigned::parseHexByteString");
    }
    size_t wordCount = (byteCount + BytesPerWord - 1) / BytesPerWord;
    BigUnsigned retval(0, wordCount);
    size_t byteNumber = 0;
    unsigned currentByte = 0;
    bool haveByte = false;
    for(size_t i = 0; i < str.size(); i++)
    {
        if(isxdigit(str[i]))
        {
            unsigned digitValue;
            if(isdigit(str[i]))
            {
                digitValue = str[i] - '0';
            }
            else if(isupper(str[i]))
            {
                digitValue = str[i] - 'A' + 0xA;
            }
            else // islower
            {
                digitValue = str[i] - 'a' + 0xA;
            }
            currentByte = currentByte * 0x10 + digitValue;
            haveByte = true;
        }
        else // str[i] == ':'
        {
            if(haveByte)
            {
                byteNumber++;
                size_t bytePos = byteCount - byteNumber;
                size_t wordPos = bytePos / BytesPerWord;
                retval.data->words[wordPos] |= ((WordType)currentByte << (8 * (bytePos % BytesPerWord)));
                haveByte = false;
                currentByte = 0;
            }
        }
    }
    if(haveByte)
    {
        byteNumber++;
        size_t bytePos = byteCount - byteNumber;
        size_t wordPos = bytePos / BytesPerWord;
        retval.data->words[wordPos] |= ((WordType)currentByte << (8 * (bytePos % BytesPerWord)));
        haveByte = false;
        currentByte = 0;
    }
    retval.normalize();
    return retval;
}

string BigUnsigned::toHexByteString() const
{
    size_t byteCount = data->size * BytesPerWord;
    while(byteCount > 1 && (((data->words[(byteCount - 1) / BytesPerWord]) >> ((byteCount - 1) % BytesPerWord) * 8) & 0xFF) == 0)
        byteCount--;
    string retval;
    retval.resize(3 * byteCount - 1, ':');
    for(size_t i = 0, j = byteCount - 1; i < byteCount; i++, j--)
    {
        unsigned currentByte = ((data->words[j / BytesPerWord]) >> (j % BytesPerWord) * 8) & 0xFF;
        unsigned highNibble = currentByte >> 4;
        unsigned lowNibble = currentByte & 0xF;
        retval[i * 3 + 0] = getHexDigit(highNibble);
        retval[i * 3 + 1] = getHexDigit(lowNibble);
    }
    return retval;
}

BigUnsigned BigUnsigned::fromByteString(string str)
{
    size_t byteCount = str.size() + 1;
    size_t wordCount = (byteCount + BytesPerWord - 1) / BytesPerWord;
    BigUnsigned retval(0, wordCount);
    size_t byteNumber = 0;
    unsigned currentByte = 1;
    size_t bytePos = byteCount - ++byteNumber;
    size_t wordPos = bytePos / BytesPerWord;
    retval.data->words[wordPos] |= ((WordType)currentByte << (8 * (bytePos % BytesPerWord)));
    for(size_t i = 0; i < str.size(); i++)
    {
        unsigned currentByte = str[i];
        size_t bytePos = byteCount - ++byteNumber;
        size_t wordPos = bytePos / BytesPerWord;
        retval.data->words[wordPos] |= ((WordType)currentByte << (8 * (bytePos % BytesPerWord)));
    }
    retval.normalize();
    return retval;
}

string BigUnsigned::toByteString() const
{
    size_t byteCount = data->size * BytesPerWord;
    while(byteCount > 1 && (((data->words[(byteCount - 1) / BytesPerWord]) >> ((byteCount - 1) % BytesPerWord) * 8) & 0xFF) == 0)
        byteCount--;
    string retval;
    retval.resize(byteCount - 1);
    for(size_t i = 0, j = byteCount - 1; i < byteCount; i++, j--)
    {
        unsigned currentByte = ((data->words[j / BytesPerWord]) >> (j % BytesPerWord) * 8) & 0xFF;
        if(i != 0)
            retval[i - 1] = currentByte;
        else if(currentByte != 1)
            handleError("number not in correct format for BigUnsigned::toByteString");
    }
    return retval;
}

const BigUnsigned & BigUnsigned::operator +=(BigUnsigned b)
{
    if(b.data->size == 1)
        return operator +=(b.data->words[0]);
    onWrite();
    size_t size = max(data->size, b.data->size) + 1;
    data->expand(size);
    bool carry = false;
    for(size_t i = 0; i < size; i++)
    {
        WordType bWord = 0;
        if(i < b.data->size)
            bWord = b.data->words[i];
        else if(!carry)
            break;
        addWithCarry(data->words[i], bWord, carry, data->words[i], carry);
    }
    normalize();
    return *this;
}

const BigUnsigned & BigUnsigned::operator +=(WordType b)
{
    onWrite();
    bool carry = false;
    addWithCarry(data->words[0], b, carry, data->words[0], carry);
    if(!carry)
        return *this;
    data->expand(data->size + 1);
    for(size_t i = 1; i < data->size; i++)
    {
        if(data->words[i]++ != WordMax)
            break;
    }
    normalize();
    return *this;
}

const BigUnsigned & BigUnsigned::operator -=(BigUnsigned b)
{
    onWrite();
    if(*this < b)
        handleError("subtraction has negative result in BigUnsigned::operator -=");
    bool borrow = false;
    size_t i;
    for(i = 0; i < b.data->size; i++)
    {
        subtractWithBorrow(data->words[i], b.data->words[i], borrow, data->words[i], borrow);
    }
    if(borrow)
    {
        for(; i < data->size; i++)
        {
            if(data->words[i]-- != 0)
                break;
        }
    }
    normalize();
    return *this;
}

const BigUnsigned & BigUnsigned::operator -=(WordType b)
{
    onWrite();
    if(*this < b)
        handleError("subtraction has negative result in BigUnsigned::operator -=");
    bool borrow = false;
    subtractWithBorrow(data->words[0], b, borrow, data->words[0], borrow);
    if(!borrow)
    {
        return *this;
    }
    for(size_t i = 1; i < data->size; i++)
    {
        if(data->words[i]-- != 0)
            break;
    }
    normalize();
    return *this;
}

BigUnsigned operator *(BigUnsigned a, WordType b)
{
    if(b == 0)
        return BigUnsigned(0);
    if(b == 1)
        return a;
    if(a.data->size == 1)
    {
        WordType highWord, lowWord;
        multiplyDoubleWord(a.data->words[0], b, highWord, lowWord);
        if(highWord == 0)
            return BigUnsigned(lowWord);
        BigUnsigned retval(lowWord, 2);
        retval.data->words[1] = highWord;
        return retval;
    }

    WordType carry = 0;
    a.onWrite();
    a.data->resize(a.data->size + 1);
    for(size_t i = 0; i < a.data->size - 1; i++)
    {
        multiplyDoubleWordAndAdd(a.data->words[i], b, carry, carry, a.data->words[i]);
    }
    a.data->words[a.data->size - 1] = carry;
    a.normalize();
    return a;
}

BigUnsigned operator *(BigUnsigned a, BigUnsigned b)
{
    if(a.data->size < b.data->size)
        swap(a, b);
    if(b.data->size == 1)
        return operator *(a, b.data->words[0]);
    BigUnsigned retval(0, a.data->size + b.data->size);
    WordType carry = 0;
    for(size_t i = 0; i < a.data->size; i++)
    {
        multiplyDoubleWordAndAdd(a.data->words[i], b.data->words[0], carry, carry, retval.data->words[i]);
    }
    retval.data->words[a.data->size] = carry;
    for(size_t i = 1; i < b.data->size; i++)
    {
        carry = 0;
        for(size_t j = 0; j < a.data->size; j++)
        {
            multiplyDoubleWordAndAddTwo(a.data->words[j], b.data->words[i], carry, retval.data->words[i + j], carry, retval.data->words[i + j]);
        }
        retval.data->words[i + a.data->size] = carry;
    }
    retval.normalize();
    return retval;
}

static void divMod(WordType dividend, WordType divisor, BigUnsigned * pquotient, BigUnsigned * premainder)
{
    if(divisor == 0)
        handleError("division by 0 in BigUnsigned::divMod");
    if(pquotient)
        *pquotient = dividend / divisor;
    if(premainder)
        *premainder = dividend % divisor;
}

static void lshiftWords(WordType dest[], WordType src[], size_t size, size_t shiftCount) // either src == dest or they aren't overlapping
{
    if(shiftCount == 0)
    {
        if(dest == src)
            return;
        for(size_t i = 0; i < size; i++)
            dest[i] = src[i];
        return;
    }
    WordType lastWord = 0;
    for(size_t i = 0; i < size; i++)
    {
        WordType word = src[i];
        lshiftDoubleWord(word, lastWord, shiftCount, dest[i]);
        lastWord = word;
    }
}

static void rshiftWords(WordType dest[], WordType src[], size_t size, size_t shiftCount) // either src == dest or they aren't overlapping
{
    if(shiftCount == 0)
    {
        if(dest == src)
            return;
        for(size_t i = 0; i < size; i++)
            dest[i] = src[i];
        return;
    }
    WordType lastWord = 0;
    for(size_t i = 0, j = size - 1; i < size; i++, j--)
    {
        WordType word = src[j];
        rshiftDoubleWord(lastWord, word, shiftCount, dest[j]);
        lastWord = word;
    }
}

static size_t countLeadingZeros(WordType v)
{
    size_t retval = 0;
    while((v & ((WordType)1 << (BitsPerWord - 1))) == 0)
    {
        retval++;
        v <<= 1;
    }
    return retval;
}

  /* Subtract x[0:len-1]*y from dest[offset:offset+len-1]. from http://www.opensource.apple.com/source/gcc/gcc-5484/libjava/gnu/java/math/MPN.java
   * All values are treated as if unsigned.
   * @return the most significant word of
   * the product, minus borrow-out from the subtraction.
   */
static WordType submul_1(WordType dest[], size_t offset, const WordType x[], size_t len, WordType y)
{
    WordType carry = 0;
    size_t j = 0;
    do
    {
        WordType prod_low;
        multiplyDoubleWordAndAdd(y, x[j], carry, carry, prod_low);
        WordType x_j = dest[offset + j];
        prod_low = x_j - prod_low;
        if (prod_low > x_j)
            carry++;
        dest[offset + j] = prod_low;
    }
    while (++j < len);
    return carry;
}


 /** Divide zds[0:nx] by y[0:ny-1]. from http://www.opensource.apple.com/source/gcc/gcc-5484/libjava/gnu/java/math/MPN.java
   * The remainder ends up in zds[0:ny-1].
   * The quotient ends up in zds[ny:nx].
   * Assumes:  nx>ny.
   * (int)y[ny-1] < 0  (i.e. most significant bit set)
   */

static void divide(WordType zds[], size_t nx, const WordType y[], size_t ny)
{
    // This is basically Knuth's formulation of the classical algorithm,
    // but translated from in scm_divbigbig in Jaffar's SCM implementation.

    // Correspondance with Knuth's notation:
    // Knuth's u[0:m+n] == zds[nx:0].
    // Knuth's v[1:n] == y[ny-1:0]
    // Knuth's n == ny.
    // Knuth's m == nx-ny.
    // Our nx == Knuth's m+n.

    // Could be re-implemented using gmp's mpn_divrem:
    // zds[nx] = mpn_divrem (&zds[ny], 0, zds, nx, y, ny).

    size_t j = nx;
    do
    {                          // loop over digits of quotient
        // Knuth's j == our nx-j.
        // Knuth's u[j:j+n] == our zds[j:j-ny].
        WordType qhat;
        if(zds[j] == y[ny - 1])
            qhat = WordMax;
        else
        {
            WordType remainder;
            divideDoubleWord(zds[j], zds[j - 1], y[ny - 1], qhat, remainder);
        }
        if(qhat != 0)
        {
            WordType borrow = submul_1(zds, j - ny, y, ny, qhat);
            WordType save = zds[j];
            if(save != borrow)
            {
                bool carry;
                do
                {
                    qhat--;
                    carry = false;
                    for (size_t i = 0; i < ny; i++)
                    {
                        addWithCarry(zds[j - ny + i], y[i], carry, zds[j - ny + i], carry);
                    }
                    if(carry)
                        zds[j]++;
                }
                while(!carry);
            }
        }
        zds[j] = qhat;
    }
    while (--j >= ny);
}

void BigUnsigned::divMod(BigUnsigned dividend, BigUnsigned divisor, BigUnsigned * pquotient, BigUnsigned * premainder)
{
    if(dividend.data->size == 1 && divisor.data->size == 1)
    {
        ::divMod(dividend.data->words[0], divisor.data->words[0], pquotient, premainder);
        return;
    }
    else if(dividend.data->size == 1)
    {
        divMod(dividend.data->words[0], divisor, pquotient, premainder);
        return;
    }
    else if(divisor.data->size == 1)
    {
        divMod(dividend, divisor.data->words[0], pquotient, premainder);
        return;
    }
    if(dividend < divisor)
    {
        if(pquotient)
            *pquotient = 0;
        if(premainder)
            *premainder = dividend;
        return;
    }
    if(dividend == divisor)
    {
        if(pquotient)
            *pquotient = 1;
        if(premainder)
            *premainder = 0;
        return;
    }
    size_t normalizationShift = countLeadingZeros(divisor.data->words[divisor.data->size - 1]);
    dividend.onWrite();
    if(normalizationShift != 0)
    {
        dividend.data->expand(dividend.data->size + 2);
        lshiftWords(dividend.data->words, dividend.data->words, dividend.data->size - 1, normalizationShift);
        divisor.onWrite();
        lshiftWords(divisor.data->words, divisor.data->words, divisor.data->size, normalizationShift);
    }
    else if(dividend.data->size == divisor.data->size)
    {
        dividend.data->expand(dividend.data->size + 2);
    }
    else
    {
        dividend.data->expand(dividend.data->size + 1);
    }
    divide(dividend.data->words, dividend.data->size - 1, divisor.data->words, divisor.data->size);
    size_t remainderSize = divisor.data->size;
    size_t quotientSize = dividend.data->size - divisor.data->size;
    if(premainder)
    {
        BigUnsigned & remainder = *premainder;
        remainder.onWrite();
        remainder.data->resize(remainderSize);
        rshiftWords(remainder.data->words, dividend.data->words, remainderSize, normalizationShift);
        remainder.normalize();
    }
    if(pquotient)
    {
        BigUnsigned & quotient = *pquotient;
        quotient.onWrite();
        quotient.data->resize(quotientSize);
        for(size_t i = 0; i < quotientSize; i++)
        {
            quotient.data->words[i] = dividend.data->words[i + divisor.data->size];
        }
    }
}

void BigUnsigned::divMod(WordType dividend, BigUnsigned divisor, BigUnsigned * pquotient, BigUnsigned * premainder)
{
    if(divisor.data->size == 1)
    {
        ::divMod(dividend, divisor.data->words[0], pquotient, premainder);
        return;
    }
    if(pquotient)
        *pquotient = 0;
    if(premainder)
        *premainder = dividend;
}

void BigUnsigned::divMod(BigUnsigned dividend, WordType divisor, BigUnsigned * pquotient, BigUnsigned * premainder)
{
    if(divisor == 0)
        handleError("division by 0 in BigUnsigned::divMod");
    if(divisor == 1)
    {
        if(premainder)
            *premainder = 0;
        if(pquotient)
            *pquotient = dividend;
        return;
    }
    if(dividend.data->size == 1)
    {
        ::divMod(dividend.data->words[0], divisor, pquotient, premainder);
        return;
    }
    if(dividend.data->size == 2)
    {
        WordType highWord = dividend.data->words[1];
        if(highWord < divisor)
        {
            WordType lowWord = dividend.data->words[0];
            WordType quotient;
            WordType remainder;
            divideDoubleWord(highWord, lowWord, divisor, quotient, remainder);
            if(pquotient)
                *pquotient = quotient;
            if(premainder)
                *premainder = remainder;
            return;
        }
    }
    if(!pquotient)
    {
        WordType remainder = 0, quotient;
        for(size_t i = 0, j = dividend.data->size - 1; i < dividend.data->size; i++, j--)
        {
            divideDoubleWord(remainder, dividend.data->words[j], divisor, quotient, remainder);
        }
        if(premainder)
            *premainder = remainder;
        return;
    }
    BigUnsigned & quotient = *pquotient;
    quotient.onWrite();
    quotient.data->expand(dividend.data->size);
    WordType remainder = 0;
    for(size_t i = 0, j = dividend.data->size - 1; i < dividend.data->size; i++, j--)
    {
        divideDoubleWord(remainder, dividend.data->words[j], divisor, quotient.data->words[j], remainder);
    }
    if(premainder)
        *premainder = remainder;
    quotient.normalize();
}

const BigUnsigned & BigUnsigned::operator <<=(size_t shiftCount)
{
    if(shiftCount == 0)
        return *this;
    if(data->size == 1 && data->words[0] == 0)
        return *this;
    onWrite();
    if(data->size == 1 && shiftCount < BitsPerWord && (data->words[0] >> (BitsPerWord - shiftCount)) == 0)
    {
        data->words[0] <<= shiftCount;
        return *this;
    }
    size_t wordCount = shiftCount / BitsPerWord;
    shiftCount %= BitsPerWord;
    size_t shiftSize = data->size;
    size_t newSize = data->size + wordCount;
    WordType topWord = data->words[data->size - 1] >> (BitsPerWord - shiftCount);
    if(topWord != 0)
        newSize++;
    data->expand(newSize);
    if(topWord != 0)
        data->words[shiftSize] = topWord;
    WordType * temp = new WordType[shiftSize];
    for(size_t i = 0; i < shiftSize; i++)
        temp[i] = data->words[i];
    lshiftWords(data->words + wordCount, temp, shiftSize, shiftCount);
    delete []temp;
    for(size_t i = 0; i < wordCount; i++)
    {
        data->words[i] = 0;
    }
    return *this;
}

const BigUnsigned & BigUnsigned::operator >>=(size_t shiftCount)
{
    if(shiftCount == 0)
        return *this;
    if(data->size == 1 && data->words[0] == 0)
        return *this;
    size_t wordCount = shiftCount / BitsPerWord;
    shiftCount %= BitsPerWord;
    if(data->size <= wordCount)
    {
        return operator =(0);
    }
    if(data->size == 1 + wordCount)
    {
        return operator =(data->words[wordCount] >> shiftCount);
    }
    onWrite();
    size_t shiftSize = data->size - wordCount;
    WordType * temp = new WordType[shiftSize];
    for(size_t i = 0; i < shiftSize; i++)
        temp[i] = data->words[i + wordCount];
    rshiftWords(data->words, temp, shiftSize, shiftCount);
    delete []temp;
    data->size = shiftSize;
    normalize();
    return *this;
}

static WordType getCharacterValue(char ch)
{
    if(isdigit(ch))
        return (WordType)ch - (WordType)'0';
    if(isupper(ch))
        return (WordType)ch - (WordType)'A' + 0xA;
    if(islower(ch))
        return (WordType)ch - (WordType)'a' + 0xA;
    return ~(WordType)0;
}

static char getCharacter(WordType v)
{
    if(v < 10)
        return (char)v + '0';
    return (char)(v - 10) + 'A';
}

BigUnsigned BigUnsigned::parse(string str, unsigned base)
{
    if(base < 2 || base > 36)
        handleError("invalid base in BigUnsigned::parse");
    BigUnsigned retval(0);
    for(size_t i = 0; i < str.size(); i++)
    {
        WordType digit = getCharacterValue(str[i]);
        if(digit >= base)
            handleError("invalid character in BigUnsigned::parse");
        retval *= (WordType)base;
        retval += digit;
    }
    return retval;
}

string BigUnsigned::toString(unsigned base) const
{
    if(base < 2 || base > 36)
        handleError("invalid base in BigUnsigned::toString");
    WordType basePower = base;
    size_t digitCount = 1;
    while(basePower < WordMax / base)
    {
        basePower *= base;
        digitCount++;
    }
    string retval;
    retval.reserve((digitCount + 1) * data->size);
    BigUnsigned v = *this;
    BigUnsigned remainder;
    WordType currentBlock;
    while(v >= basePower)
    {
        divMod(v, basePower, v, remainder);
        currentBlock = (WordType)remainder;
        for(size_t i = 0; i < digitCount; i++)
        {
            WordType digit = currentBlock % base;
            currentBlock /= base;
            retval.push_back(getCharacter(digit));
        }
    }
    currentBlock = (WordType)v;
    while(currentBlock != 0)
    {
        WordType digit = currentBlock % base;
        currentBlock /= base;
        retval.push_back(getCharacter(digit));
    }
    if(retval.size() >= 2)
    {
        for(size_t i = 0, j = retval.size() - 1; j > i; j--, i++)
        {
            std::swap(retval[i], retval[j]);
        }
    }
    if(retval == "")
        retval = "0";
    return retval;
}

static char getBase64Character(WordType v)
{
    if(v < 26)
        return 'A' + v;
    v -= 26;
    if(v < 26)
        return 'a' + v;
    v -= 26;
    if(v < 10)
        return '0' + v;
    else if(v == 10)
        return '+';
    return '/';
}

static WordType getBase64Value(char ch)
{
    if(isupper(ch))
        return ch - 'A';
    if(islower(ch))
        return ch - 'a' + 26;
    if(isdigit(ch))
        return ch - '0' + 26 * 2;
    if(ch == '+')
        return 62;
    if(ch == '/')
        return 63;
    return WordMax;
}

BigUnsigned BigUnsigned::parseBase64(string str)
{
    while(str.size() > 0 && str[str.size() - 1] == '=')
        str.erase(str.size() - 1);
    BigUnsigned retval(0);
    for(size_t i = 0; i < str.size(); i++)
    {
        WordType v = getBase64Value(str[i]);
        if(v >= 64)
            handleError("invalid base64 character");
        retval <<= 6;
        retval |= v;
    }
    return retval;
}

string BigUnsigned::toBase64() const
{
    string retval;
    retval.reserve(((BitsPerWord * data->size + 5) / 6));
    bool first = true;
    for(BigUnsigned v = *this; v != (WordType)0 || first; v >>= 6, first = false)
    {
        retval.push_back(getBase64Character((WordType)(v & (WordType)0x3F)));
    }
    if(retval.size() >= 2)
    {
        for(size_t i = 0, j = retval.size() - 1; j > i; j--, i++)
        {
            std::swap(retval[i], retval[j]);
        }
    }
    return retval;
}