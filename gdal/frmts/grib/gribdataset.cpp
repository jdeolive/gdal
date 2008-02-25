/******************************************************************************
 * $Id: gribdataset.cpp,v 1.9 2005/05/05 15:54:48 fwarmerdam Exp $
 *
 * Project:  GRIB Driver
 * Purpose:  GDALDataset driver for GRIB translator for read support
 * Author:   Bas Retsios, retsios@itc.nl
 *
 ******************************************************************************
 * Copyright (c) 2007, ITC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ******************************************************************************
 * 
 */

#include "gdal_pam.h"

#include "degrib18/degrib/degrib2.h"
#include "degrib18/degrib/inventory.h"
#include "degrib18/degrib/myerror.h"
#include "degrib18/degrib/filedatasource.h"
#include "degrib18/degrib/memorydatasource.h"

#include "ogr_spatialref.h"

CPL_CVSID("$Id: gribdataset.cpp,v 1.9 2005/05/05 15:54:48 fwarmerdam Exp $");

CPL_C_START
void	GDALRegister_GRIB(void);
CPL_C_END

/************************************************************************/
/* ==================================================================== */
/*				GRIBDataset				*/
/* ==================================================================== */
/************************************************************************/

class GRIBRasterBand;

class GRIBDataset : public GDALPamDataset
{
    friend class GRIBRasterBand;

  public:
		GRIBDataset();
		~GRIBDataset();
    
    static GDALDataset *Open( GDALOpenInfo * );

    CPLErr 	GetGeoTransform( double * padfTransform );
    const char *GetProjectionRef();
	private:
		void SetGribMetaData(grib_MetaData* meta);
    FILE	*fp;
    char  *pszProjection;
		char  *pszDescription;
    OGRCoordinateTransformation *poTransform;
    double adfGeoTransform[6]; // Calculate and store once as GetGeoTransform may be called multiple times
};

/************************************************************************/
/* ==================================================================== */
/*                            GRIBRasterBand                             */
/* ==================================================================== */
/************************************************************************/

class GRIBRasterBand : public GDALPamRasterBand
{
    friend class GRIBDataset;
    
public:
    GRIBRasterBand( GRIBDataset*, int, sInt4, int, char* );
    virtual ~GRIBRasterBand();
    virtual CPLErr IReadBlock( int, int, void * );
    virtual const char *GetDescription() const;
private:
    static void ReadGribData( DataSource &, sInt4, int, double**, grib_MetaData**);
    sInt4 start;
    int subgNum;
    char *longFstLevel;
    double * m_Grib_Data;
    grib_MetaData* m_Grib_MetaData;
};


/************************************************************************/
/*                           GRIBRasterBand()                            */
/************************************************************************/

GRIBRasterBand::GRIBRasterBand( GRIBDataset *poDS, int nBand, sInt4 start, int subgNum, char *longFstLevel )
  : m_Grib_Data(NULL)
  , m_Grib_MetaData(NULL)
{
    this->poDS = poDS;
    this->nBand = nBand;
    this->start = start;
    this->subgNum = subgNum;
    this->longFstLevel = CPLStrdup(longFstLevel);

    eDataType = GDT_Float64; // let user do -ot Float32 if needed for saving space, GRIB contains Float64 (though not fully utilized most of the time)

    nBlockXSize = poDS->nRasterXSize;
    nBlockYSize = 1;
}

/************************************************************************/
/*                         GetDescription()                             */
/************************************************************************/

const char * GRIBRasterBand::GetDescription() const
{
    if( longFstLevel == NULL )
        return GDALPamRasterBand::GetDescription();
    else
        return longFstLevel;
}
 
/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr GRIBRasterBand::IReadBlock( int nBlockXOff, int nBlockYOff,
                                   void * pImage )

{
    if (!m_Grib_Data)
    {
        GRIBDataset *poGDS = (GRIBDataset *) poDS;

        FileDataSource grib_fp (poGDS->fp);

        ReadGribData(grib_fp, start, subgNum, &m_Grib_Data, &m_Grib_MetaData);
    }

    // Somehow this decoder guarantees us that the image is upside-down (GRIB scan mode 0100). Therefore reverse it again.
    memcpy(pImage, m_Grib_Data + nRasterXSize * (nRasterYSize - nBlockYOff - 1), nRasterXSize * sizeof(double));

    return CE_None;
}

/************************************************************************/
/*                            ReadGribData()                            */
/************************************************************************/

void GRIBRasterBand::ReadGribData( DataSource & fp, sInt4 start, int subgNum, double** data, grib_MetaData** metaData)
{
    /* Initialisation, for calling the ReadGrib2Record function */
    sInt4 f_endMsg = 1;  /* 1 if we read the last grid in a GRIB message, or we haven't read any messages. */
    // int subgNum = 0;     /* The subgrid in the message that we are interested in. */
    sChar f_unit = 2;        /* None = 0, English = 1, Metric = 2 */
    double majEarth = 0;     /* -radEarth if < 6000 ignore, otherwise use this to
                              * override the radEarth in the GRIB1 or GRIB2
                              * message.  Needed because NCEP uses 6371.2 but GRIB1 could only state 6367.47. */
    double minEarth = 0;     /* -minEarth if < 6000 ignore, otherwise use this to
                              * override the minEarth in the GRIB1 or GRIB2 message. */
    sChar f_SimpleVer = 4;   /* Which version of the simple NDFD Weather table to
                              * use. (1 is 6/2003) (2 is 1/2004) (3 is 2/2004)
                              * (4 is 11/2004) (default 4) */
    LatLon lwlf;         /* lower left corner (cookie slicing) -lwlf */
    LatLon uprt;         /* upper right corner (cookie slicing) -uprt */
    IS_dataType is;      /* Un-parsed meta data for this GRIB2 message. As well as some memory used by the unpacker. */

    lwlf.lat = -100; // lat == -100 instructs the GRIB decoder that we don't want a subgrid

    IS_Init (&is);


    /* Read GRIB message from file position "start". */
    fp.DataSourceFseek(start, SEEK_SET);
    uInt4 grib_DataLen = 0;  /* Size of Grib_Data. */
    *metaData = new grib_MetaData();
    MetaInit (*metaData);
    ReadGrib2Record (fp, f_unit, data, &grib_DataLen, *metaData, &is, subgNum,
                     majEarth, minEarth, f_SimpleVer, &f_endMsg, &lwlf, &uprt);

    char * errMsg = errSprintf(NULL); // no intention to show errors, just swallow it and free the memory
    if( errMsg != NULL )
        CPLDebug( "GRIB", "%s", errMsg );
    free(errMsg);
    IS_Free(&is);
}

/************************************************************************/
/*                           ~GRIBRasterBand()                          */
/************************************************************************/

GRIBRasterBand::~GRIBRasterBand()
{
    CPLFree(longFstLevel);
    if (m_Grib_Data)
        free (m_Grib_Data);
    if (m_Grib_MetaData)
    {
        MetaFree( m_Grib_MetaData );
        delete m_Grib_MetaData;
    }
}

/************************************************************************/
/* ==================================================================== */
/*				GRIBDataset				*/
/* ==================================================================== */
/************************************************************************/

GRIBDataset::GRIBDataset()

{
  poTransform = NULL;
  pszProjection = CPLStrdup("");
  adfGeoTransform[0] = 0.0;
  adfGeoTransform[1] = 1.0;
  adfGeoTransform[2] = 0.0;
  adfGeoTransform[3] = 0.0;
  adfGeoTransform[4] = 0.0;
  adfGeoTransform[5] = 1.0;
}

/************************************************************************/
/*                            ~GRIBDataset()                             */
/************************************************************************/

GRIBDataset::~GRIBDataset()

{
    FlushCache();
    if( fp != NULL )
        VSIFClose( fp );
		
    CPLFree( pszProjection );
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr GRIBDataset::GetGeoTransform( double * padfTransform )

{
    memcpy( padfTransform,  adfGeoTransform, sizeof(double) * 6 );
    return CE_None;
}

/************************************************************************/
/*                          GetProjectionRef()                          */
/************************************************************************/

const char *GRIBDataset::GetProjectionRef()

{
    return pszProjection;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *GRIBDataset::Open( GDALOpenInfo * poOpenInfo )

{
/* -------------------------------------------------------------------- */
/*      A fast "probe" on the header that is partially read in memory.  */
/* -------------------------------------------------------------------- */
    if( poOpenInfo->fp == NULL)
        return NULL;

    char *buff = NULL;
    uInt4 buffLen = 0;
    sInt4 sect0[SECT0LEN_WORD];
    uInt4 gribLen;
    int version;
    MemoryDataSource mds (poOpenInfo->pabyHeader, poOpenInfo->nHeaderBytes);
    if (ReadSECT0 (mds, &buff, &buffLen, -1, sect0, &gribLen, &version) < 0) {
        free (buff);
        char * errMsg = errSprintf(NULL);
        if( errMsg != NULL )
            CPLDebug( "GRIB", "%s", errMsg );
        free(errMsg);
        return NULL;
    }
    free(buff);
    
/* -------------------------------------------------------------------- */
/*      Create a corresponding GDALDataset.                             */
/* -------------------------------------------------------------------- */
    GRIBDataset 	*poDS;

    poDS = new GRIBDataset();

    poDS->fp = poOpenInfo->fp;
    poOpenInfo->fp = NULL;
    
/* -------------------------------------------------------------------- */
/*      Read the header.                                                */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/*      Make an inventory of the GRIB file.                             */
/* The inventory does not contain all the information needed for        */
/* creating the RasterBands (especially the x and y size), therefore    */
/* the first GRIB band is also read for some additional metadata.       */
/* The band-data that is read is stored into the first RasterBand,      */
/* simply so that the same portion of the file is not read twice.       */
/* -------------------------------------------------------------------- */
    
    VSIFSeek( poDS->fp, 0, SEEK_SET );

    FileDataSource grib_fp (poDS->fp);

    inventoryType *Inv = NULL;  /* Contains an GRIB2 message inventory of the file */
    uInt4 LenInv = 0;        /* size of Inv (also # of GRIB2 messages) */
    int msgNum =0;          /* The messageNumber during the inventory. */

    if (GRIB2Inventory (grib_fp, &Inv, &LenInv, 0, &msgNum) <= 0 )
    {
        CPLError( CE_Failure, CPLE_OpenFailed, 
                  "%s is a grib file, but no raster dataset was successfully identified.",
                  poOpenInfo->pszFilename );
        delete poDS;
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Create band objects.                                            */
/* -------------------------------------------------------------------- */
    for (uInt4 i = 0; i < LenInv; ++i)
    {
        uInt4 bandNr = i+1;
        if (bandNr == 1)
        {
            // important: set DataSet extents before creating first RasterBand in it
            double * data = NULL;
            grib_MetaData* metaData;
            GRIBRasterBand::ReadGribData(grib_fp, 0, Inv[i].subgNum, &data, &metaData);
            if (metaData->gds.Nx < 1 || metaData->gds.Ny < 1 )
            {
                CPLError( CE_Failure, CPLE_OpenFailed, 
                          "%s is a grib file, but no raster dataset was successfully identified.",
                          poOpenInfo->pszFilename );
                delete poDS;
                return NULL;
            }

            poDS->SetGribMetaData(metaData); // set the DataSet's x,y size, georeference and projection from the first GRIB band
            GRIBRasterBand* gribBand = new GRIBRasterBand( poDS, bandNr, Inv[i].start, Inv[i].subgNum, Inv[i].longFstLevel);
            gribBand->m_Grib_Data = data;
            gribBand->m_Grib_MetaData = metaData;
            poDS->SetBand( bandNr, gribBand);
        }
        else
            poDS->SetBand( bandNr, new GRIBRasterBand( poDS, bandNr, Inv[i].start, Inv[i].subgNum, Inv[i].longFstLevel));
        GRIB2InventoryFree (Inv + i);
    }
    free (Inv);

/* -------------------------------------------------------------------- */
/*      Initialize any PAM information.                                 */
/* -------------------------------------------------------------------- */
    poDS->SetDescription( poOpenInfo->pszFilename );
    poDS->TryLoadXML();

    return( poDS );
}

/************************************************************************/
/*                            SetMetadata()                             */
/************************************************************************/

void GRIBDataset::SetGribMetaData(grib_MetaData* meta)
{
    nRasterXSize = meta->gds.Nx;
    nRasterYSize = meta->gds.Ny;

    OGRSpatialReference oSRS; // the projection of the image

    switch(meta->gds.projType)
    {
      case GS3_LATLON:
      case GS3_GAUSSIAN_LATLON:
          // No projection, only latlon system (geographic)
          break;
      case GS3_MERCATOR:
        oSRS.SetMercator(meta->gds.meshLat, meta->gds.orientLon,
                         1.0, 0.0, 0.0);
        break;
      case GS3_POLAR:
        oSRS.SetPS(meta->gds.meshLat, meta->gds.orientLon,
                   meta->gds.scaleLat1,
                   0.0, 0.0);
        break;
      case GS3_LAMBERT:
        oSRS.SetLCC(meta->gds.scaleLat1, meta->gds.scaleLat2,
                    0.0, meta->gds.orientLon,
                    0.0, 0.0); // set projection
        break;
			

      case GS3_ORTHOGRAPHIC:

        //oSRS.SetOrthographic(0.0, meta->gds.orientLon,
        //											meta->gds.lon2, meta->gds.lat2);
        //oSRS.SetGEOS(meta->gds.orientLon, meta->gds.stretchFactor, meta->gds.lon2, meta->gds.lat2);
        oSRS.SetGEOS(  0, 35785831, 0, 0 ); // hardcoded for now, I don't know yet how to parse the meta->gds section
        break;
      case GS3_EQUATOR_EQUIDIST:
        break;
      case GS3_AZIMUTH_RANGE:
        break;
    }

    double a = meta->gds.majEarth * 1000.0; // in meters
    double b = meta->gds.minEarth * 1000.0;
    if( a == 0 && b == 0 )
    {
        a = 6377563.396;
        b = 6356256.910;
    }

    if (meta->gds.f_sphere)
    {
        oSRS.SetGeogCS( "Coordinate System imported from GRIB file",
                        NULL,
                        "Sphere",
                        a, 0.0 );
    }
    else
    {
        double fInv = a/(a-b);
        oSRS.SetGeogCS( "Coordinate System imported from GRIB file",
                        NULL,
                        "Spheroid imported from GRIB file",
                        a, fInv );
    }

    OGRSpatialReference oLL; // construct the "geographic" part of oSRS
    oLL.CopyGeogCSFrom( &oSRS );

    double rMinX;
    double rMaxY;
    double rPixelSizeX;
    double rPixelSizeY;
    if (meta->gds.projType == GS3_ORTHOGRAPHIC)
    {
        //rMinX = -meta->gds.Dx * (meta->gds.Nx / 2); // This is what should work, but it doesn't .. Dx seems to have an inverse relation with pixel size
        //rMaxY = meta->gds.Dy * (meta->gds.Ny / 2);
        const double geosExtentInMeters = 11137496.552; // hardcoded for now, assumption: GEOS projection, full disc (like MSG)
        rMinX = -(geosExtentInMeters / 2);
        rMaxY = geosExtentInMeters / 2;
        rPixelSizeX = geosExtentInMeters / meta->gds.Nx;
        rPixelSizeY = geosExtentInMeters / meta->gds.Ny;
    }
    else if( oSRS.IsProjected() )
    {
        rMinX = meta->gds.lon1; // longitude in degrees, to be transformed to meters (or degrees in case of latlon)
        rMaxY = meta->gds.lat1; // latitude in degrees, to be transformed to meters 
        OGRCoordinateTransformation *poTransformLLtoSRS = OGRCreateCoordinateTransformation( &(oLL), &(oSRS) );
        if ((poTransformLLtoSRS != NULL) && poTransformLLtoSRS->Transform( 1, &rMinX, &rMaxY )) // transform it to meters
        {
            if (meta->gds.scan == GRIB2BIT_2) // Y is minY, GDAL wants maxY
                rMaxY += (meta->gds.Ny - 1) * meta->gds.Dy; // -1 because we GDAL needs the coordinates of the centre of the pixel
            rPixelSizeX = meta->gds.Dx;
            rPixelSizeY = meta->gds.Dy;
        }
        else
        {
            rMinX = 0.0;
            rMaxY = 0.0;
            
            rPixelSizeX = 1.0;
            rPixelSizeY = -1.0;
            
            oSRS.Clear();

            CPLError( CE_Warning, CPLE_AppDefined,
                      "Unable to perform coordinate transformations, so the correct\n"
                      "projected geotransform could not be deduced from the lat/long\n"
                      "control points.  Defaulting to ungeoreferenced." );
        }
        delete poTransformLLtoSRS;
    }
    else
    {
        rMinX = meta->gds.lon1; // longitude in degrees, to be transformed to meters (or degrees in case of latlon)
        rMaxY = meta->gds.lat1; // latitude in degrees, to be transformed to meters 

        if (meta->gds.scan == GRIB2BIT_2) // Y is minY, GDAL wants maxY
            rMaxY += (meta->gds.Ny - 1) * meta->gds.Dy; // -1 because we GDAL needs the coordinates of the centre of the pixel
        rPixelSizeX = meta->gds.Dx;
        rPixelSizeY = meta->gds.Dy;
    }

    adfGeoTransform[0] = rMinX;
    adfGeoTransform[3] = rMaxY;
    adfGeoTransform[1] = rPixelSizeX;
    adfGeoTransform[5] = -rPixelSizeY;

    CPLFree( pszProjection );
    pszProjection = NULL;
    oSRS.exportToWkt( &(pszProjection) );
}

/************************************************************************/
/*                         GDALRegister_GRIB()                          */
/************************************************************************/

void GDALRegister_GRIB()

{
    GDALDriver	*poDriver;

    if( GDALGetDriverByName( "GRIB" ) == NULL )
    {
        poDriver = new GDALDriver();
        
        poDriver->SetDescription( "GRIB" );
        poDriver->SetMetadataItem( GDAL_DMD_LONGNAME, 
                                   "GRIdded Binary (.grb)" );
        poDriver->SetMetadataItem( GDAL_DMD_HELPTOPIC, 
                                   "frmt_grib.html" );
        poDriver->SetMetadataItem( GDAL_DMD_EXTENSION, "grb" );

        poDriver->pfnOpen = GRIBDataset::Open;

        GetGDALDriverManager()->RegisterDriver( poDriver );
    }
}
