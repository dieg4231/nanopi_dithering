#include "jpeg.h"

#include <jpeglib.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <cmath>


#define BLUE_H 0X38488d
#define GREEN_H 0X547a49
#define RED_H 0X9f4b4e
#define BLACK_H 0X242933
#define YELLOW_H 0Xc9d168
#define ORANGE_H 0Xb55d4c
#define WHITE_H 0Xd3dde4

enum colors { BLUE = 0, GREEN, RED, BLACK, YELLOW, ORANGE, WHITE };



namespace marengo
{
namespace jpeg
{

Image::Image( const std::string& fileName )
{
    // Creating a custom deleter for the decompressInfo pointer
    // to ensure ::jpeg_destroy_compress() gets called even if
    // we throw out of this function.
    auto dt = []( ::jpeg_decompress_struct *ds )
            {
                ::jpeg_destroy_decompress( ds );
            };
    std::unique_ptr<::jpeg_decompress_struct, decltype(dt)> decompressInfo(
            new ::jpeg_decompress_struct,
            dt
            );

    // Note this is a shared pointer as we can share this 
    // between objects which have copy constructed from each other
    m_errorMgr = std::make_shared<::jpeg_error_mgr>();

    // Using fopen here ( and in save() ) because libjpeg expects
    // a FILE pointer.
    // We store the FILE* in a unique_ptr so we can also use the custom
    // deleter here to ensure fclose() gets called even if we throw.
    auto fdt = []( FILE* fp )
            {
                fclose( fp );
            };
    std::unique_ptr<FILE, decltype(fdt)> infile(
            fopen( fileName.c_str(), "rb" ),
            fdt
            );
    if ( infile.get() == NULL )
    {
        throw std::runtime_error( "Could not open " + fileName );
    }

    decompressInfo->err = ::jpeg_std_error( m_errorMgr.get() );
    // Note this usage of a lambda to provide our own error handler
    // to libjpeg. If we do not supply a handler, and libjpeg hits
    // a problem, it just prints the error message and calls exit().
    m_errorMgr->error_exit = [](::j_common_ptr cinfo)
        {
            char jpegLastErrorMsg[JMSG_LENGTH_MAX];
            // Call the function pointer to get the error message
            ( *( cinfo->err->format_message ) )
                ( cinfo, jpegLastErrorMsg );
            throw std::runtime_error( jpegLastErrorMsg );
        };
    ::jpeg_create_decompress( decompressInfo.get() );

    // Read the file:
    ::jpeg_stdio_src( decompressInfo.get(), infile.get() );

    int rc = ::jpeg_read_header( decompressInfo.get(), TRUE );
    if (rc != 1)
    {
        throw std::runtime_error(
            "File does not seem to be a normal JPEG"
            );
    }
    ::jpeg_start_decompress( decompressInfo.get() );

    m_width       = decompressInfo->output_width;
    m_height      = decompressInfo->output_height;
    m_pixelSize   = decompressInfo->output_components;
    m_colourSpace = decompressInfo->out_color_space;

    size_t row_stride = m_width * m_pixelSize;

    m_bitmapData.clear();
    m_bitmapData.reserve( m_height );

    while ( decompressInfo->output_scanline < m_height )
    {
        std::vector<uint8_t> vec(row_stride);
        uint8_t* p = vec.data();
        ::jpeg_read_scanlines( decompressInfo.get(), &p, 1 );
        m_bitmapData.push_back( vec );
    }
    ::jpeg_finish_decompress( decompressInfo.get() );
}

// Copy constructor
Image::Image( const Image& rhs )
{
    m_errorMgr      = rhs.m_errorMgr;
    m_bitmapData    = rhs.m_bitmapData;
    m_width         = rhs.m_width;
    m_height        = rhs.m_height;
    m_pixelSize     = rhs.m_pixelSize;
    m_colourSpace   = rhs.m_colourSpace;
}

Image::~Image()
{
}

void Image::save( const std::string& fileName, int quality ) const
{
    if ( quality < 0 )
    {
        quality = 0;
    }
    if ( quality > 100 )
    {
        quality = 100;
    }
    FILE* outfile = fopen( fileName.c_str(), "wb" );
    if ( outfile == NULL )
    {
        throw std::runtime_error(
            "Could not open " + fileName + " for writing"
            );
    }
    // Creating a custom deleter for the compressInfo pointer
    // to ensure ::jpeg_destroy_compress() gets called even if
    // we throw out of this function.
    auto dt = []( ::jpeg_compress_struct *cs )
            {
                ::jpeg_destroy_compress( cs );
            };
    std::unique_ptr<::jpeg_compress_struct, decltype(dt)> compressInfo(
            new ::jpeg_compress_struct,
            dt );
    ::jpeg_create_compress( compressInfo.get() );
    ::jpeg_stdio_dest( compressInfo.get(), outfile);
    compressInfo->image_width = m_width;
    compressInfo->image_height = m_height;
    compressInfo->input_components = m_pixelSize;
    compressInfo->in_color_space =
        static_cast<::J_COLOR_SPACE>( m_colourSpace );
    compressInfo->err = ::jpeg_std_error( m_errorMgr.get() );
    ::jpeg_set_defaults( compressInfo.get() );
    ::jpeg_set_quality( compressInfo.get(), quality, TRUE );
    ::jpeg_start_compress( compressInfo.get(), TRUE);
    for ( auto const& vecLine : m_bitmapData )
    {
        ::JSAMPROW rowPtr[1];
        // Casting const-ness away here because the jpeglib
        // call expects a non-const pointer. It presumably
        // doesn't modify our data.
        rowPtr[0] = const_cast<::JSAMPROW>( vecLine.data() );
        ::jpeg_write_scanlines(
            compressInfo.get(),
            rowPtr,
            1
            );
    }
    ::jpeg_finish_compress( compressInfo.get() );
    fclose( outfile );
}

void Image::savePpm( const std::string& fileName ) const
{
    std::ofstream ofs( fileName, std::ios::out | std::ios::binary );
    if ( ! ofs )
    {
        throw std::runtime_error(
            "Could not open " + fileName + " for saving"
            );
    }
    // Write the header
    ofs << "P6 " << m_width << " " << m_height << " 255\n";
    for ( auto& v : m_bitmapData )
    {
        ofs.write( reinterpret_cast<const char *>(v.data()), v.size() );
    }
    ofs.close();
}

std::vector<uint8_t> Image::getPixel( size_t x, size_t y ) const
{
    if ( y >= m_bitmapData.size() )
    {
        throw std::out_of_range( "Y value too large" );
    }
    if ( x >= m_bitmapData[0].size() / m_pixelSize )
    {
        throw std::out_of_range( "X value too large" );
    }
    std::vector<uint8_t> vec;
    for ( size_t n = 0; n < m_pixelSize; ++n )
    {
        vec.push_back( m_bitmapData[ y ][ x * m_pixelSize + n ] );
    }
    return vec;
}

uint8_t Image::getLuminance( size_t x, size_t y ) const
{
    auto vec = getPixel( x, y );
    if ( vec.size() == 1 )
    {   // monochrome image
        return vec[0];
    }
    if ( vec.size() == 3 )
    {
        // fast approximation of luminance:
        return static_cast<uint8_t>(
            ( vec[0] * 2 + vec[1] * 3 + vec[2] ) / 6
            );
    }
    return 0;
}

std::vector<uint8_t>
Image::getAverage( size_t x, size_t y, size_t boxSize ) const
{
    if ( boxSize > m_width )
    {
        throw std::out_of_range( "Box size is greater than image width" );
    }
    if ( boxSize > m_height )
    {
        throw std::out_of_range( "Box size is greater than image height" );
    }
    if ( x + boxSize  >= m_width )
    {
        x = m_width - boxSize;
    }
    if ( y + boxSize >= m_height )
    {
        y = m_height - boxSize;
    }
    // running totals
    size_t r{ 0 }; // we just use this one for mono images
    size_t g{ 0 };
    size_t b{ 0 };
    for ( size_t row = y; row < y + boxSize; ++row )
    {
        for ( size_t col = x; col < x + boxSize; ++col )
        {
            auto vec = getPixel( col, row );
            r += vec[0];
            if ( vec.size() == 3 )
            {
                g += vec[1];
                b += vec[2];
            }
        }
    }
    std::vector<uint8_t> retVec;
    r /= ( boxSize * boxSize );
    retVec.push_back( r );
    if ( m_pixelSize == 3 )
    {
        g /= ( boxSize * boxSize );
        retVec.push_back( g );
        b /= ( boxSize * boxSize );
        retVec.push_back( b );
    }
    return retVec;
}

void Image::shrink( size_t newWidth )
{
    if ( newWidth >= m_width )
    {
        return;
    }

    if ( newWidth == 0 )
    {
        throw std::out_of_range( "New width cannot be zero" );
    }

    // We process the original bitmap line by line rather than
    // calling getAverage() on every (new) pixel to ensure we make the
    // most of data already in existing cache lines & attempt to
    // allow branch prediction to work optimally. This has resulted
    // in a three-times speedup when shrinking a 21Mpx file.

    float scaleFactor = static_cast<float>(newWidth) / m_width;
    size_t newHeight = scaleFactor * m_height;
    std::vector<std::vector<uint8_t>> vecNewBitmap;
    vecNewBitmap.reserve( newHeight );

    // Yes, I probably could do a rolling average
    std::vector<size_t> runningTotals( newWidth * m_pixelSize );
    std::vector<size_t> runningCounts( newWidth * m_pixelSize );
    size_t oldRow = 0;
    for ( size_t row = 0; row < m_height; ++row )
    {
        for ( size_t col = 0; col < m_width * m_pixelSize; ++col )
        {
            size_t idx = scaleFactor * col;
            runningTotals[ idx ] += m_bitmapData[row][col];
            ++runningCounts[ idx ];
        }
        if ( static_cast<size_t>( scaleFactor * row ) > oldRow )
        {
            oldRow = scaleFactor * row;
            std::vector<uint8_t> newLine;
            newLine.reserve( newWidth * m_pixelSize );
            for ( size_t i = 0; i < newWidth * m_pixelSize; ++i )
            {
                newLine.push_back( runningTotals[i] / runningCounts[i] );
                runningTotals[i] = 0;
                runningCounts[i] = 0;
            }
            vecNewBitmap.push_back( newLine );
        }
    }
    m_bitmapData = vecNewBitmap;
    m_height = m_bitmapData.size();
    m_width = m_bitmapData[0].size() / m_pixelSize;
}

void Image::test(  )
{
    

    // We process the original bitmap line by line rather than
    // calling getAverage() on every (new) pixel to ensure we make the
    // most of data already in existing cache lines & attempt to
    // allow branch prediction to work optimally. This has resulted
    // in a three-times speedup when shrinking a 21Mpx file.

    std::vector<std::vector<uint8_t>> vecNewBitmap;
    vecNewBitmap.reserve( m_height );


    for ( size_t row = 0; row < m_height; ++row )
    {
        std::vector<uint8_t> newLine;
        newLine.reserve( m_height * m_pixelSize );
        int cta =0;
        for ( size_t col = 0; col < m_width * m_pixelSize ; ++col )
        {
            if(col < (m_width * m_pixelSize)/2  ){
                newLine.push_back(m_bitmapData[row][col]);
            }
            else{
                if( m_pixelSize == 1 )
                {
                    newLine.push_back(0XFF);
                }else{
                    if(cta==0)  newLine.push_back(0XFF);
                    if(cta==1)  newLine.push_back(0X00);
                    if(cta==2)  newLine.push_back(0X00);
                    cta++;
                    if(cta==3)  cta=0;
                }
            }
            
        }
        
        vecNewBitmap.push_back( newLine );
        
    }
    m_bitmapData = vecNewBitmap;
    m_height = m_bitmapData.size();
    m_width = m_bitmapData[0].size() / m_pixelSize;
}

void Image::fsd(  )
{
    



    std::vector<std::vector<uint8_t>> vecNewBitmap;
    vecNewBitmap.reserve( m_height );

    // to grayscale
    for ( size_t row = 0; row < m_height; ++row )
    {
        std::vector<uint8_t> newLine;
        newLine.reserve( m_width  );
        for ( size_t col = 0; col < m_width * m_pixelSize; col+=3 )
        {
            newLine.push_back( (m_bitmapData[row][col] + m_bitmapData[row][col+1]  + m_bitmapData[row][col+2] )/3 );
        }        
        vecNewBitmap.push_back( newLine );
    }

    //fsd
    uint8_t newPixel;
    uint8_t oldPixel;
    uint8_t error;
    for ( size_t row = 0; row < m_height; ++row )
    {
        std::vector<uint8_t> newLine;
        newLine.reserve( m_width * m_pixelSize  );
    
        for ( size_t col = 0; col < m_width; ++col )
        {
            if( col == 0 || col == m_width - 1 || (row == m_height -1)  ){
                vecNewBitmap[row][col] = 0XFF;
            }else{
                oldPixel = vecNewBitmap[row][col];
                newPixel =  oldPixel <= 128 ? 0XFF : 0X00;
                error = ( oldPixel - newPixel );
                vecNewBitmap[row     ][col + 1 ] += error*7/16;
                vecNewBitmap[row + 1 ][col + 1 ] += error*1/16;
                vecNewBitmap[row + 1 ][col     ] += error*5/16; 
                vecNewBitmap[row + 1 ][col - 1 ] += error*3/16; 
                vecNewBitmap[row][col] = newPixel;
            }   

        }        
    }

    std::vector<std::vector<uint8_t>> vecNewBitmapD;
    vecNewBitmapD.reserve( m_height );
    // to  3 chanels
    for ( size_t row = 0; row < m_height; ++row )
    {
        std::vector<uint8_t> newLine;
        newLine.reserve( m_width * m_pixelSize  );
        for ( size_t col = 0; col < m_width; ++col )
        {
            newLine.push_back( vecNewBitmap[row][col]);
            newLine.push_back( vecNewBitmap[row][col]);
            newLine.push_back( vecNewBitmap[row][col]);
        }        
        vecNewBitmapD.push_back( newLine );
    }






    m_pixelSize=3;
    m_bitmapData = vecNewBitmapD;
    m_height = m_bitmapData.size();
    m_width = m_bitmapData[0].size() / m_pixelSize;
}

void Image::fsdColor(  )
{
     std::vector<std::vector<uint8_t>>  palet = { {0X38,0X48,0X8d}, 
                                                    {0X54,0X7a,0X49},
                                                    {0X9f,0X4b,0X4e},
                                                    {0X24,0X29,0X33},
                                                    {0Xc9,0Xd1,0X68},
                                                    {0Xb5,0X5d,0X4c},
                                                    {0Xd3,0Xdd,0Xe4}
                                                };
  
    int16_t deltaR;
    int16_t deltaG;
    int16_t deltaB;
    int16_t rHat;

    int idColor;
    float auxDistance;
    float shortDistance;
    


    //fsd_color
    int16_t newPixelR;
    int16_t newPixelG;
    int16_t newPixelB;
    int16_t oldPixelR;
    int16_t oldPixelG;
    int16_t oldPixelB;
    int16_t errorR;
    int16_t errorG;
    int16_t errorB;
    for ( size_t row = 0; row < m_height; ++row )
    {    
        for ( size_t col = 0; col < m_width * m_pixelSize ; col+=3 )
        {
            //srgb
            uint8_t r ;
            uint8_t g ;
            uint8_t b ;
            

            if( col == 0 || col >= ((m_width * m_pixelSize )- m_pixelSize) || (row == m_height -1)  ){
                m_bitmapData[row][col] = palet[0][0];
                m_bitmapData[row][col+1] = palet[0][1];
                m_bitmapData[row][col+2] = palet[0][2];
            }else{

                r = m_bitmapData[row][col];
                g = m_bitmapData[row][col + 1];
                b = m_bitmapData[row][col + 2];

                //Getting new color from palet by srgb distance
                for(int idxPalet = 0; idxPalet < 7; idxPalet++){
                    deltaR = r - palet[idxPalet][0];
                    deltaG = g - palet[idxPalet][1];
                    deltaB = b - palet[idxPalet][2];
                    rHat = (r + palet[idxPalet][0]) / 2;

                    if(rHat < 128){
                        auxDistance = sqrt( (2 * deltaR*deltaR) + (4 * deltaG*deltaG) + (3 * deltaB*deltaB) );
                    }else{
                        auxDistance = sqrt( (3 * deltaR*deltaR) + (4 * deltaG*deltaG) + (2 * deltaB*deltaB) );
                    }

                    if(idxPalet == 0){
                        shortDistance = auxDistance;
                        idColor = idxPalet;
                    }else{
                        if(shortDistance > auxDistance){
                            shortDistance = auxDistance;
                            idColor = idxPalet;
                        }
                    }
                }


                oldPixelR = m_bitmapData[row][col];
                oldPixelG = m_bitmapData[row][col + 1];
                oldPixelB = m_bitmapData[row][col + 2];

                newPixelR = palet[idColor][0];
                newPixelG = palet[idColor][1];
                newPixelB = palet[idColor][2];

                errorR = oldPixelR - newPixelR;
                errorG = oldPixelG - newPixelG;
                errorB = oldPixelB - newPixelB;

                m_bitmapData[row     ][col + 0 + 3] += errorR*7/16;
                m_bitmapData[row     ][col + 1 + 3] += errorG*7/16;
                m_bitmapData[row     ][col + 2 + 3] += errorB*7/16;

                m_bitmapData[row + 1 ][col + 0 + 3 ] += errorR*1/16;
                m_bitmapData[row + 1 ][col + 1 + 3 ] += errorG*1/16;
                m_bitmapData[row + 1 ][col + 2 + 3 ] += errorB*1/16;
                
                m_bitmapData[row + 1 ][col  + 0    ] += errorR*5/16; 
                m_bitmapData[row + 1 ][col  + 1    ] += errorG*5/16; 
                m_bitmapData[row + 1 ][col  + 2    ] += errorB*5/16; 

                m_bitmapData[row + 1 ][col - 0 - 3 ] += errorR*3/16; 
                m_bitmapData[row + 1 ][col + 1 - 3 ] += errorG*3/16; 
                m_bitmapData[row + 1 ][col + 2 - 3 ] += errorB*3/16; 

                m_bitmapData[row][col] = newPixelR;
                m_bitmapData[row][col+1] = newPixelG;
                m_bitmapData[row][col+2] = newPixelB;
            }        
        }
    }

    m_pixelSize=3;
    m_bitmapData = m_bitmapData;
    m_height = m_bitmapData.size();
    m_width = m_bitmapData[0].size() / m_pixelSize;
}

void Image::expand( size_t newWidth )
{
    if ( newWidth <= m_width )
    {
        return;
    }

    float scaleFactor = static_cast<float>(newWidth) / m_width;
    size_t newHeight = scaleFactor * m_height;
    std::vector<std::vector<uint8_t>> vecNewBitmap;
    vecNewBitmap.reserve( newHeight );

    for ( size_t row = 0; row < newHeight; ++row )
    {
        size_t oldRow = row / scaleFactor;
        std::vector<uint8_t> vecNewLine( newWidth * m_pixelSize );
        for ( size_t col = 0; col < newWidth; ++col )
        {
            size_t oldCol = col / scaleFactor;
            for ( size_t n = 0; n < m_pixelSize; ++n )
            {
                vecNewLine[ col * m_pixelSize + n ] =
                    m_bitmapData[ oldRow ][ oldCol * m_pixelSize + n ];
            }
        }
        vecNewBitmap.push_back( vecNewLine );
    }
    m_bitmapData = vecNewBitmap;
    m_height = m_bitmapData.size();
    m_width = m_bitmapData[0].size() / m_pixelSize;
}

void Image::resize( size_t newWidth )
{
    if ( newWidth < m_width )
    {
        shrink( newWidth );
    }
    else if ( newWidth > m_width )
    {
        expand( newWidth );
    }
}


} // namespace jpeg
} // namespace marengo

