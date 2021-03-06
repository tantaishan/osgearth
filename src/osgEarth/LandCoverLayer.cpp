/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2016 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/LandCoverLayer>
#include <osgEarth/ImageUtils>
#include <osgEarth/Registry>
#include <osgEarth/Map>

using namespace osgEarth;

#define LC "[LandCoverLayer] "

REGISTER_OSGEARTH_LAYER(land_cover, LandCoverLayer);

namespace
{
    osg::Vec2 getSplatCoords(const TileKey& key, float baseLOD, const osg::Vec2& covUV)
    {
        osg::Vec2 out;

        float dL = (float)key.getLOD() - baseLOD;
        float factor = pow(2.0f, dL);
        float invFactor = 1.0/factor;
        out.set( covUV.x()*invFactor, covUV.y()*invFactor ); 

        // For upsampling we need to calculate an offset as well
        if ( factor >= 1.0 )
        {
            unsigned wide, high;
            key.getProfile()->getNumTiles(key.getLOD(), wide, high);

            float tileX = (float)key.getTileX();
            float tileY = (float)(wide-1-key.getTileY()); // swap Y. (not done in the shader version.)

            osg::Vec2 a( floor(tileX*invFactor), floor(tileY*invFactor) );
            osg::Vec2 b( a.x()*factor, a.y()*factor );
            osg::Vec2 c( (a.x()+1.0f)*factor, (a.y()+1.0f)*factor );
            osg::Vec2 offset( (tileX-b.x())/(c.x()-b.x()), (tileY-b.y())/(c.y()-b.y()) );

            out += offset;
        }

        return out;
    }
    
    typedef std::vector<int> CodeMap;

    struct ILayer 
    {
        GeoImage  image;
        float     scale;
        osg::Vec2 bias;
        bool      valid;
        float     warp;
        ImageUtils::PixelReader* read;
        unsigned* codeTable;

        ILayer() : valid(true), read(0L), scale(1.0f), warp(0.0f) { }

        ~ILayer() { if (read) delete read; }

        void load(const TileKey& key, ImageLayer* sourceLayer, ProgressCallback* progress)
        {
            if ( sourceLayer->getEnabled() && sourceLayer->getVisible() && sourceLayer->isKeyInLegalRange(key) )
            {
                for(TileKey k = key; k.valid() && !image.valid(); k = k.createParentKey())
                {
                    image = sourceLayer->createImage(k, progress);
                } 
            }

            valid = image.valid();

            if ( valid )
            {
                scale = key.getExtent().width() / image.getExtent().width();
                bias.x() = (key.getExtent().xMin() - image.getExtent().xMin()) / image.getExtent().width();
                bias.y() = (key.getExtent().yMin() - image.getExtent().yMin()) / image.getExtent().height();

                read = new ImageUtils::PixelReader(image.getImage());

                // cannot interpolate coverage data:
                read->setBilinear( false );

                //warp = sourceWarp;
            }
        }
    };

    // Constructs a code map (int to int) for a coverage layer. We will use this
    // code map to map coverage layer codes to dictionary codes.
    void buildCodeMap(const LandCoverCoverageLayer* coverage, CodeMap& codemap)
    {
        if (!coverage) {
            OE_WARN << LC << "ILLEGAL: coverage not passed to buildCodeMap\n";
            return;
        }
        if (!coverage->getDictionary()) {
            OE_WARN << LC << "ILLEGAL: coverage dictionary not set in buildCodeMap\n";
            return;
        }

        int highestValue = 0;

        for (LandCoverValueMappingVector::const_iterator k = coverage->getMappings().begin();
            k != coverage->getMappings().end();
            ++k)
        {
            const LandCoverValueMapping* mapping = k->get();
            int value = mapping->getValue();
            if (value > highestValue)
                highestValue = value;
        }

        codemap.assign(highestValue+1, -1);

        for (LandCoverValueMappingVector::const_iterator k = coverage->getMappings().begin();
            k != coverage->getMappings().end();
            ++k)
        {
            const LandCoverValueMapping* mapping = k->get();
            int value = mapping->getValue();
            const LandCoverClass* lcClass = coverage->getDictionary()->getClass(mapping->getLandCoverClassName());
            if (lcClass)
            {
                codemap[value] = lcClass->getValue();
            }
        }
    }

    typedef std::vector<osg::ref_ptr<LandCoverCoverageLayer> > LandCoverCoverageLayerVector;

    /*
     * TileSource that provides GeoImage's to the LandCoverLayer.
     */
    class LandCoverTileSource : public TileSource
    {
    public:
        LandCoverTileSource(const LandCoverLayerOptions& options);

    public: // TileSource
        Status initialize(const osgDB::Options* readOptions);

        osg::Image* createImage(const TileKey& key, ProgressCallback* progress);
        
        CachePolicy getCachePolicyHint() const {
            return CachePolicy::NO_CACHE;
        }

        const LandCoverLayerOptions* _options;
        const LandCoverLayerOptions& options() const { return *_options; }

        void setDictionary(LandCoverDictionary*);
        
        // image layers, one per data source
        LandCoverCoverageLayerVector _coverages;

        // code maps (vector index is the source code; value is the destination code)
        std::vector<CodeMap> _codemaps;

        // todo
        std::vector<float> _warps;

        osg::ref_ptr<osgDB::Options> _readOptions;  

        osg::ref_ptr<LandCoverDictionary> _lcDictionary;

        friend class LandCoverLayer;
    };


    LandCoverTileSource::LandCoverTileSource(const LandCoverLayerOptions& options) :
        TileSource(options),
        _options(&options)
    {
        //nop
    }

    Status
    LandCoverTileSource::initialize(const osgDB::Options* readOptions)
    {
        const Profile* profile = getProfile();
        if ( !profile )
        {
            profile = osgEarth::Registry::instance()->getGlobalGeodeticProfile();
            setProfile( profile );
        }

        for(unsigned i=0; i<options().coverages().size(); ++i)
        {
            LandCoverCoverageLayerOptions coverageOptions = options().coverages()[i];
            if (coverageOptions.enabled() == false)
                continue;

            coverageOptions.cachePolicy() = CachePolicy::NO_CACHE; // TODO: yes? no?

            // Create the coverage layer:
            LandCoverCoverageLayer* layer = new LandCoverCoverageLayer( coverageOptions );

            // Set up and open it.^
            layer->setTargetProfileHint( profile );
            layer->setReadOptions(readOptions);
            const Status& s = layer->open();
            if (s.isOK())
            {
                _coverages.push_back(layer);
                _codemaps.resize(_codemaps.size()+1);
                OE_INFO << LC << "Opened coverage \"" << layer->getName() << "\"\n";
            }
            else
            {
                OE_WARN << "Layer \"" << layer->getName() << "\": " << s.toString() << std::endl;
            }

            //Config conf = ilo.getConfig();
            //_warps[i] = conf.value("warp", options().warpFactor().get());
        }
        
#if 0
        // set up the noise generator.
        const float F[4] = { 4.0f, 16.0f, 4.0f, 8.0f };
        const float P[4] = { 0.8f,  0.6f, 0.8f, 0.9f };
        const float L[4] = { 2.2f,  1.7f, 3.0f, 4.0f };
    
        // Configure the noise function:
        _noiseGen.setNormalize  ( true );
        _noiseGen.setRange      ( 0.0, 1.0 );
        _noiseGen.setFrequency  ( F[0] );
        _noiseGen.setPersistence( P[0] );
        _noiseGen.setLacunarity ( L[0] );
        _noiseGen.setOctaves    ( 8 );
#endif

        return STATUS_OK;
    }

    void
    LandCoverTileSource::setDictionary(LandCoverDictionary* lcd)
    {
        _lcDictionary = lcd;

        for (unsigned i = 0; i<_coverages.size(); ++i)
        {
            _coverages[i]->setDictionary(lcd);
            buildCodeMap(_coverages[i].get(), _codemaps[i]);
        }
    }
    
    //TODO: overriding createImage directly like this will bypass caching.
    //      This is a temporary solution; need to refactor.
    osg::Image*
    LandCoverTileSource::createImage(const TileKey& key, ProgressCallback* progress)
    {
        if ( _coverages.empty() )
            return 0L;

        std::vector<ILayer> layers(_coverages.size());

        // Allocate the new coverage image; it will contain unnormalized values.
        osg::Image* out = new osg::Image();
        ImageUtils::markAsUnNormalized(out, true);

        // Allocate a suitable format:
        GLenum dataType;
        GLint  internalFormat;
    
        if ( options().bits().isSetTo(16u) )
        {
            // 16-bit float:
            dataType       = GL_FLOAT;
            internalFormat = GL_LUMINANCE16F_ARB;
        }
        else //if ( _options.bits().isSetTo(32u) )
        {
            // 32-bit float:
            dataType       = GL_FLOAT;
            internalFormat = GL_LUMINANCE32F_ARB;
        }
    
        int tilesize = getPixelsPerTile();

        out->allocateImage(tilesize, tilesize, 1, GL_LUMINANCE, dataType);
        out->setInternalTextureFormat(internalFormat);

        //float noiseLOD = options().baseLOD().get();
        //float warp     = options().warpFactor().get();

        osg::Vec2 cov;    // coverage coordinates
        //float     noise;  // noise value
        //osg::Vec2 noiseCoords;

        ImageUtils::PixelWriter write( out );

        float du = 1.0f / (float)(out->s()-1);
        float dv = 1.0f / (float)(out->t()-1);

        osg::Vec4 nodata;
        if (internalFormat == GL_LUMINANCE16F_ARB)
            nodata.set(-32768, -32768, -32768, -32768);
        else
            nodata.set(NO_DATA_VALUE, NO_DATA_VALUE, NO_DATA_VALUE, NO_DATA_VALUE);

        for(float u=0.0f; u<=1.0f; u+=du)
        {
            for(float v=0.0f; v<=1.0f; v+=dv)
            {
                bool wrotePixel = false;
                for(int L = layers.size()-1; L >= 0 && !wrotePixel; --L)
                {
                    ILayer& layer = layers[L];
                    if ( !layer.valid )
                        continue;

                    if ( !layer.image.valid() )
                        layer.load(key, _coverages[L], progress);

                    if ( !layer.valid )
                        continue;

                    osg::Vec2 cov(layer.scale*u + layer.bias.x(), layer.scale*v + layer.bias.y());

                    if ( cov.x() >= 0.0f && cov.x() <= 1.0f && cov.y() >= 0.0f && cov.y() <= 1.0f )
                    {
                        // Noise is like a repeating overlay at the noiseLOD. So sample it using
                        // straight U/V tile coordinates.
                        //noiseCoords = getSplatCoords( key, noiseLOD, osg::Vec2(u,v) );
                        //noise = getNoise( _noiseGen, noiseCoords );
                        //cov = warpCoverageCoords(cov, noise, layer.warp);

                        osg::Vec4 texel = (*layer.read)(cov.x(), cov.y());
                        if ( texel.r() != NO_DATA_VALUE )
                        {
                            if (texel.r() < 1.0f)
                            {
                                // normalized code; convert
                                int code = (int)(texel.r()*255.0f);
                                if (code < _codemaps[L].size())
                                {
                                    int value = _codemaps[L][code];
                                    if (value >= 0)
                                    {
                                        texel.r() = (float)value;
                                        //texel.r() = ((float)value)/255.0f;
                                        write.f(texel, u, v);
                                        wrotePixel = true;
                                    }
                                }
                            }
                            else
                            {
                                // unnormalized
                                int code = (int)texel.r();
                                if (code < _codemaps[L].size() && _codemaps[L][code] >= 0)
                                {
                                    texel.r() = (float)_codemaps[L][code];
                                    write.f(texel, u, v);
                                    wrotePixel = true;
                                }
                            }
                        }
                    }
                }

                if ( !wrotePixel )
                {
                    write.f(nodata, u, v);
                }
            }
        }

        return out;
    }
}

//........................................................................

#undef  LC
#define LC "[LandCoverLayerOptions] "

LandCoverLayerOptions::LandCoverLayerOptions(const ConfigOptions& options) :
ImageLayerOptions(options)
{
    fromConfig(_conf);
}

void
LandCoverLayerOptions::fromConfig(const Config& conf)
{
    //conf.getIfSet("warp", _warp);
    //conf.getIfSet("base_lod", _baseLOD);
    conf.getIfSet("bits", _bits);
    
    //_dictionary = new LandCoverDictionary(conf.child("land_cover_dictionary"));

    ConfigSet layerConfs = conf.child("coverages").children("coverage");
    for (ConfigSet::const_iterator i = layerConfs.begin(); i != layerConfs.end(); ++i)
    {
        _coverages.push_back(LandCoverCoverageLayerOptions(*i));
    }
}

Config
LandCoverLayerOptions::getConfig() const
{
    Config conf = ImageLayerOptions::getConfig();
    conf.key() = "land_cover";

    //conf.addIfSet("warp", _warp);
    //conf.addIfSet("base_lod", _baseLOD);
    conf.addIfSet("bits", _bits);    

    if (_coverages.size() > 0)
    {
        Config images("images");
        for (std::vector<LandCoverCoverageLayerOptions>::const_iterator i = _coverages.begin();
            i != _coverages.end();
            ++i)
        {
            images.add("image", i->getConfig());
        }
        conf.add(images);
    }

    return conf;
}

//...........................................................................

#undef  LC
#define LC "[LandCoverLayer] "


LandCoverLayer::LandCoverLayer() :
ImageLayer(&_optionsConcrete),
_options(&_optionsConcrete)
{
    init();
}

LandCoverLayer::LandCoverLayer(const LandCoverLayerOptions& options) :
ImageLayer(&_optionsConcrete),
_options(&_optionsConcrete),
_optionsConcrete(options)
{
    init();
}

void
LandCoverLayer::init()
{
    options().coverage() = true;
    options().visible() = false;
    options().shared() = true;
    ImageLayer::init();
}

void
LandCoverLayer::addedToMap(const Map* map)
{
    // Find a land cover dictionary if there is one.
    // There had better be one, or we are not going to get very far!
    // This is called after createTileSource, so the TileSource should exist at this point.
    // Note. If the land cover dictionary isn't already in the Map...this will fail! (TODO)
    // Consider a LayerListener. (TODO)
    _lcDictionary = map->getLayer<LandCoverDictionary>();
    if (_lcDictionary.valid() && getTileSource())
    {
        static_cast<LandCoverTileSource*>(getTileSource())->setDictionary(_lcDictionary.get());
    }
    else
    {
        OE_WARN << LC << "Did not find a LandCoverDictionary in the Map!\n";
    }
}

TileSource*
LandCoverLayer::createTileSource()
{
    return new LandCoverTileSource(options());
}
