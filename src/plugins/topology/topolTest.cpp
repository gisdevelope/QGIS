/***************************************************************************
  topolTest.cpp
  TOPOLogy checker
  -------------------
         date                 : May 2009
         copyright            : (C) 2009 by Vita Cizek
         email                : weetya (at) gmail.com

 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "topolTest.h"

#include "qgsvectorlayer.h"
#include "qgsfeatureiterator.h"
#include "qgsmaplayer.h"
#include "qgsmapcanvas.h"
#include "qgsgeometry.h"
#include "qgsfeature.h"
#include "qgsspatialindex.h"
#include "qgisinterface.h"
#include "qgslogger.h"
#include "qgsmessagelog.h"
#include <cmath>
#include <set>
#include <map>

static bool _canExportToGeos( const QgsGeometry &geom )
{
  GEOSGeometry *geosGeom = geom.exportToGeos();
  if ( geosGeom )
  {
    GEOSGeom_destroy_r( QgsGeometry::getGEOSHandler(), geosGeom );
    return true;
  }
  return false;
}

topolTest::topolTest( QgisInterface *qgsIface )
{
  qgsInterface = qgsIface;
  mTestCanceled = false;

  // one layer tests
  mTopologyRuleMap.insert( tr( "must not have invalid geometries" ),
                           TopologyRule( &topolTest::checkValid,
                                         false, false,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PointGeometry << QgsWkbTypes::PolygonGeometry << QgsWkbTypes::LineGeometry ) );

  mTopologyRuleMap.insert( tr( "must not have dangles" ),
                           TopologyRule( &topolTest::checkDanglingLines,
                                         false, false,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::LineGeometry ) );

  mTopologyRuleMap.insert( tr( "must not have duplicates" ),
                           TopologyRule( &topolTest::checkDuplicates,
                                         false, true,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PointGeometry << QgsWkbTypes::PolygonGeometry << QgsWkbTypes::LineGeometry ) );

  mTopologyRuleMap.insert( tr( "must not have pseudos" ),
                           TopologyRule( &topolTest::checkPseudos,
                                         false, false,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::LineGeometry ) );

  mTopologyRuleMap.insert( tr( "must not overlap" ),
                           TopologyRule( &topolTest::checkOverlaps,
                                         false, true,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PolygonGeometry ) );

  mTopologyRuleMap.insert( tr( "must not have gaps" ),
                           TopologyRule( &topolTest::checkGaps,
                                         false, false,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PolygonGeometry ) );

  mTopologyRuleMap.insert( tr( "must not have multi-part geometries" ),
                           TopologyRule( &topolTest::checkMultipart,
                                         false, false,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PointGeometry << QgsWkbTypes::PolygonGeometry << QgsWkbTypes::LineGeometry ) );

  // two layer tests
  mTopologyRuleMap.insert( tr( "must not overlap with" ),
                           TopologyRule( &topolTest::checkOverlapWithLayer,
                                         true, true,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PolygonGeometry,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PolygonGeometry ) );

  mTopologyRuleMap.insert( tr( "must be covered by" ),
                           TopologyRule( &topolTest::checkPointCoveredBySegment,
                                         true, true,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PointGeometry,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::LineGeometry << QgsWkbTypes::PolygonGeometry ) );

  mTopologyRuleMap.insert( tr( "must be covered by endpoints of" ),
                           TopologyRule( &topolTest::checkPointCoveredByLineEnds,
                                         true, true,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PointGeometry,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::LineGeometry ) );

  mTopologyRuleMap.insert( tr( "end points must be covered by" ),
                           TopologyRule( &topolTest::checkyLineEndsCoveredByPoints,
                                         true, true,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::LineGeometry,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PointGeometry ) );

  mTopologyRuleMap.insert( tr( "must be inside" ),
                           TopologyRule( &topolTest::checkPointInPolygon,
                                         true, true,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PointGeometry,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PolygonGeometry ) );

  mTopologyRuleMap.insert( tr( "must contain" ),
                           TopologyRule( &topolTest::checkPolygonContainsPoint,
                                         true, true,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PolygonGeometry,
                                         QList<QgsWkbTypes::GeometryType>() << QgsWkbTypes::PointGeometry ) );
}

topolTest::~topolTest()
{
  QMap<QString, QgsSpatialIndex *>::const_iterator lit = mLayerIndexes.constBegin();
  for ( ; lit != mLayerIndexes.constEnd(); ++lit )
    delete *lit;
}

void topolTest::setTestCanceled()
{
  mTestCanceled = true;
}

bool topolTest::testCanceled()
{
  if ( mTestCanceled )
  {
    mTestCanceled = false;
    return true;
  }

  return false;
}

ErrorList topolTest::checkDanglingLines( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  Q_UNUSED( layer2 );

  int i = 0;
  ErrorList errorList;
  QgsFeature f;

  if ( layer1->geometryType() != QgsWkbTypes::LineGeometry )
  {
    return errorList;
  }

  QList<FeatureLayer>::iterator it;

  qDebug() << mFeatureList1.count();

  QgsPointXY startPoint;
  QgsPointXY endPoint;

  std::multimap<QgsPointXY, QgsFeatureId, PointComparer> endVerticesMap;

  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );

    if ( testCanceled() )
      break;

    QgsGeometry g1 = it->feature.geometry();

    if ( g1.isNull() )
    {
      QgsMessageLog::logMessage( tr( "First geometry invalid in dangling line test." ), tr( "Topology plugin" ) );
      continue;
    }

    if ( !_canExportToGeos( g1 ) )
    {
      QgsMessageLog::logMessage( tr( "Failed to import first geometry into GEOS in dangling line test." ), tr( "Topology plugin" ) );
      continue;
    }

    if ( g1.isMultipart() )
    {
      QgsMultiPolylineXY lines = g1.asMultiPolyline();
      for ( int m = 0; m < lines.count(); m++ )
      {
        QgsPolylineXY line = lines[m];
        startPoint = line[0];
        endPoint = line[line.size() - 1];

        endVerticesMap.insert( std::pair<QgsPointXY, QgsFeatureId>( startPoint, it->feature.id() ) );
        endVerticesMap.insert( std::pair<QgsPointXY, QgsFeatureId>( endPoint, it->feature.id() ) );

      }
    }
    else
    {
      QgsPolylineXY polyline = g1.asPolyline();
      startPoint = polyline[0];
      endPoint = polyline[polyline.size() - 1];
      endVerticesMap.insert( std::pair<QgsPointXY, QgsFeatureId>( startPoint, it->feature.id() ) );
      endVerticesMap.insert( std::pair<QgsPointXY, QgsFeatureId>( endPoint, it->feature.id() ) );
    }
  }

  QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );


  for ( std::multimap<QgsPointXY, QgsFeatureId, PointComparer>::iterator pointIt = endVerticesMap.begin(), end = endVerticesMap.end(); pointIt != end; pointIt = endVerticesMap.upper_bound( pointIt->first ) )
  {
    QgsPointXY p = pointIt->first;
    QgsFeatureId k = pointIt->second;

    size_t repetitions = endVerticesMap.count( p );

    //QgsGeometry* extentPoly =
    if ( repetitions == 1 )
    {

      QgsGeometry conflictGeom = QgsGeometry::fromPointXY( p );
      if ( isExtent )
      {
        if ( canvasExtentPoly.disjoint( conflictGeom ) )
        {
          continue;
        }
      }

      QgsRectangle bBox = conflictGeom.boundingBox();
      QgsFeature feat;

      FeatureLayer ftrLayer1;
      //need to fetch attributes?? being safe side by fetching..
      layer1->getFeatures( QgsFeatureRequest().setFilterFid( k ) ).nextFeature( feat );
      ftrLayer1.feature = feat;
      ftrLayer1.layer = layer1;

      QList<FeatureLayer> errorFtrLayers;
      errorFtrLayers << ftrLayer1 << ftrLayer1;

      TopolErrorDangle *err = new TopolErrorDangle( bBox, conflictGeom, errorFtrLayers );
      errorList << err;

    }
  }
  return errorList;
}

ErrorList topolTest::checkDuplicates( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  Q_UNUSED( layer2 );
  //TODO: multilines - check all separate pieces
  int i = 0;
  ErrorList errorList;

  QList<QgsFeatureId> duplicateIds;

  QgsSpatialIndex *index = mLayerIndexes[layer1->id()];

  QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );

  QMap<QgsFeatureId, FeatureLayer>::const_iterator it;
  for ( it = mFeatureMap2.constBegin(); it != mFeatureMap2.constEnd(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );

    QgsFeatureId currentId = it->feature.id();

    if ( duplicateIds.contains( currentId ) )
    {
      //is already a duplicate geometry..skip..
      continue;
    }

    if ( testCanceled() )
      break;

    QgsGeometry g1 = it->feature.geometry();
    QgsRectangle bb = g1.boundingBox();

    QList<QgsFeatureId> crossingIds;
    crossingIds = index->intersects( bb );

    QList<QgsFeatureId>::ConstIterator cit = crossingIds.constBegin();
    QList<QgsFeatureId>::ConstIterator crossingIdsEnd = crossingIds.constEnd();

    bool duplicate = false;

    for ( ; cit != crossingIdsEnd; ++cit )
    {
      duplicate = false;
      // skip itself
      if ( mFeatureMap2[*cit].feature.id() == it->feature.id() )
        continue;

      QgsGeometry g2 = mFeatureMap2[*cit].feature.geometry();
      if ( g2.isNull() )
      {
        QgsMessageLog::logMessage( tr( "Invalid second geometry in duplicate geometry test." ), tr( "Topology plugin" ) );
        continue;
      }

      if ( !_canExportToGeos( g2 ) )
      {
        QgsMessageLog::logMessage( tr( "Failed to import second geometry into GEOS in duplicate geometry test." ), tr( "Topology plugin" ) );
        continue;
      }

      if ( g1.isGeosEqual( g2 ) )
      {
        duplicate = true;
        duplicateIds.append( mFeatureMap2[*cit].feature.id() );
      }

      if ( duplicate )
      {


        QList<FeatureLayer> fls;
        fls << *it << *it;
        QgsGeometry conflict( g1 );

        if ( isExtent )
        {
          if ( canvasExtentPoly.disjoint( conflict ) )
          {
            continue;
          }
          if ( canvasExtentPoly.crosses( conflict ) )
          {
            conflict = conflict.intersection( canvasExtentPoly );
          }
        }

        TopolErrorDuplicates *err = new TopolErrorDuplicates( bb, conflict, fls );

        errorList << err;
      }

    }

  }
  return errorList;
}

ErrorList topolTest::checkOverlaps( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  Q_UNUSED( layer2 );
  int i = 0;
  ErrorList errorList;

  // could be enabled for lines and points too
  // so duplicate rule may be removed?

  if ( layer1->geometryType() != QgsWkbTypes::PolygonGeometry )
  {
    return errorList;
  }

  QList<QgsFeatureId> *duplicateIds = new QList<QgsFeatureId>();

  QgsSpatialIndex *index = mLayerIndexes[layer1->id()];
  if ( !index )
  {
    qDebug() << "no index present";
    delete duplicateIds;
    return errorList;
  }

  QMap<QgsFeatureId, FeatureLayer>::const_iterator it;
  for ( it = mFeatureMap2.constBegin(); it != mFeatureMap2.constEnd(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );

    QgsFeatureId currentId = it->feature.id();

    if ( duplicateIds->contains( currentId ) )
    {
      //is already a duplicate geometry..skip..
      continue;
    }

    if ( testCanceled() )
      break;

    QgsGeometry g1 = it->feature.geometry();

    if ( !g1.isGeosValid() )
    {
      qDebug() << "invalid geometry(g1) found..skipping.." << it->feature.id();
      continue;
    }

    QgsRectangle bb = g1.boundingBox();

    QList<QgsFeatureId> crossingIds;
    crossingIds = index->intersects( bb );

    QList<QgsFeatureId>::ConstIterator cit = crossingIds.begin();
    QList<QgsFeatureId>::ConstIterator crossingIdsEnd = crossingIds.end();

    bool duplicate = false;

    QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );

    for ( ; cit != crossingIdsEnd; ++cit )
    {
      duplicate = false;
      // skip itself
      if ( mFeatureMap2[*cit].feature.id() == it->feature.id() )
        continue;

      QgsGeometry g2 = mFeatureMap2[*cit].feature.geometry();
      if ( g2.isNull() )
      {
        QgsMessageLog::logMessage( tr( "Invalid second geometry in overlaps test." ), tr( "Topology plugin" ) );
        continue;
      }

      if ( !_canExportToGeos( g2 ) )
      {
        QgsMessageLog::logMessage( tr( "Failed to import second geometry into GEOS in overlaps test." ), tr( "Topology plugin" ) );
        continue;
      }

      if ( !g2.isGeosValid() )
      {
        QgsMessageLog::logMessage( tr( "Skipping invalid second geometry of feature %1 in overlaps test." ).arg( it->feature.id() ), tr( "Topology plugin" ) );
        continue;
      }


      qDebug() << "checking overlap for" << it->feature.id();
      if ( g1.overlaps( g2 ) )
      {
        duplicate = true;
        duplicateIds->append( mFeatureMap2[*cit].feature.id() );
      }

      if ( duplicate )
      {
        QList<FeatureLayer> fls;
        fls << *it << *it;
        QgsGeometry conflictGeom = g1.intersection( g2 );

        if ( isExtent )
        {
          if ( canvasExtentPoly.disjoint( conflictGeom ) )
          {
            continue;
          }
          if ( canvasExtentPoly.crosses( conflictGeom ) )
          {
            conflictGeom = conflictGeom.intersection( canvasExtentPoly );
          }
        }

        TopolErrorOverlaps *err = new TopolErrorOverlaps( bb, conflictGeom, fls );

        errorList << err;
      }

    }
  }

  delete duplicateIds;

  return errorList;
}

ErrorList topolTest::checkGaps( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  Q_UNUSED( layer2 );

  int i = 0;
  ErrorList errorList;
  GEOSContextHandle_t geosctxt = QgsGeometry::getGEOSHandler();

  // could be enabled for lines and points too
  // so duplicate rule may be removed?

  if ( layer1->geometryType() != QgsWkbTypes::PolygonGeometry )
  {
    return errorList;
  }

  QList<FeatureLayer>::iterator it;
  QgsGeometry g1;

  QList<GEOSGeometry *> geomList;

  qDebug() << mFeatureList1.count() << " features in list!";
  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    qDebug() << "reading features-" << i;

    if ( !( ++i % 100 ) )
    {
      emit progress( i );
    }

    if ( testCanceled() )
    {
      break;
    }

    g1 = it->feature.geometry();

    if ( g1.isNull() )
    {
      continue;
    }

    if ( !_canExportToGeos( g1 ) )
    {
      continue;
    }

    if ( !g1.isGeosValid() )
    {
      qDebug() << "invalid geometry found..skipping.." << it->feature.id();
      continue;
    }

    if ( g1.isMultipart() )
    {
      QgsMultiPolygonXY polys = g1.asMultiPolygon();
      for ( int m = 0; m < polys.count(); m++ )
      {
        QgsPolygonXY polygon = polys[m];

        QgsGeometry polyGeom = QgsGeometry::fromPolygonXY( polygon );

        geomList.push_back( polyGeom.exportToGeos() );
      }

    }
    else
    {
      geomList.push_back( g1.exportToGeos() );
    }
  }

  GEOSGeometry **geomArray = new GEOSGeometry*[geomList.size()];
  for ( int i = 0; i < geomList.size(); ++i )
  {
    //qDebug() << "filling geometry array-" << i;
    geomArray[i] = geomList.at( i );
  }

  qDebug() << "creating geometry collection-";

  if ( geomList.isEmpty() )
  {
    //qDebug() << "geometry list is empty!";
    delete [] geomArray;
    return errorList;
  }

  GEOSGeometry *collection = nullptr;
  collection = GEOSGeom_createCollection_r( geosctxt, GEOS_MULTIPOLYGON, geomArray, geomList.size() );


  qDebug() << "performing cascaded union..might take time..-";
  GEOSGeometry *unionGeom = GEOSUnionCascaded_r( geosctxt, collection );
  //delete[] geomArray;

  QgsGeometry test;
  test.fromGeos( unionGeom );


  //qDebug() << "wktmerged - " << test.exportToWkt();

  QString extentWkt = test.boundingBox().asWktPolygon();
  QgsGeometry extentGeom = QgsGeometry::fromWkt( extentWkt );
  QgsGeometry bufferExtent = extentGeom.buffer( 2, 3 );

  //qDebug() << "extent wkt - " << bufferExtent->exportToWkt();

  QgsGeometry diffGeoms = bufferExtent.difference( test );
  if ( !diffGeoms )
  {
    qDebug() << "difference result 0-";
    return errorList;
  }

  //qDebug() << "difference gometry - " << diffGeoms->exportToWkt();

  QVector<QgsGeometry> geomColl = diffGeoms.asGeometryCollection();

  QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );

  for ( int i = 1; i < geomColl.count() ; ++i )
  {
    QgsGeometry conflictGeom = geomColl[i];
    if ( isExtent )
    {
      if ( canvasExtentPoly.disjoint( conflictGeom ) )
      {
        continue;
      }
      if ( canvasExtentPoly.crosses( conflictGeom ) )
      {
        conflictGeom = conflictGeom.intersection( canvasExtentPoly );
      }
    }
    QgsRectangle bBox = conflictGeom.boundingBox();
    FeatureLayer ftrLayer1;
    ftrLayer1.layer = layer1;
    QList<FeatureLayer> errorFtrLayers;
    errorFtrLayers << ftrLayer1 << ftrLayer1;
    TopolErrorGaps *err = new TopolErrorGaps( bBox, conflictGeom, errorFtrLayers );
    errorList << err;
  }

  return errorList;
}

ErrorList topolTest::checkPseudos( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  Q_UNUSED( layer2 );

  int i = 0;
  ErrorList errorList;
  QgsFeature f;

  if ( layer1->geometryType() != QgsWkbTypes::LineGeometry )
  {
    return errorList;
  }

  QList<FeatureLayer>::iterator it;

  qDebug() << mFeatureList1.count();

  QgsPointXY startPoint;
  QgsPointXY endPoint;

  std::multimap<QgsPointXY, QgsFeatureId, PointComparer> endVerticesMap;

  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );

    if ( testCanceled() )
      break;

    QgsGeometry g1 = it->feature.geometry();

    if ( g1.isNull() )
    {
      QgsMessageLog::logMessage( tr( "Skipping invalid first geometry in pseudo line test." ), tr( "Topology plugin" ) );
      continue;
    }

    if ( !_canExportToGeos( g1 ) )
    {
      QgsMessageLog::logMessage( tr( "Failed to import first geometry into GEOS in pseudo line test." ), tr( "Topology plugin" ) );
      continue;
    }

    if ( g1.isMultipart() )
    {
      QgsMultiPolylineXY lines = g1.asMultiPolyline();
      for ( int m = 0; m < lines.count(); m++ )
      {
        QgsPolylineXY line = lines[m];
        startPoint = line[0];
        endPoint = line[line.size() - 1];

        endVerticesMap.insert( std::pair<QgsPointXY, QgsFeatureId>( startPoint, it->feature.id() ) );
        endVerticesMap.insert( std::pair<QgsPointXY, QgsFeatureId>( endPoint, it->feature.id() ) );

      }
    }
    else
    {
      QgsPolylineXY polyline = g1.asPolyline();
      startPoint = polyline[0];
      endPoint = polyline[polyline.size() - 1];
      endVerticesMap.insert( std::pair<QgsPointXY, QgsFeatureId>( startPoint, it->feature.id() ) );
      endVerticesMap.insert( std::pair<QgsPointXY, QgsFeatureId>( endPoint, it->feature.id() ) );
    }
  }


  QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );


  for ( std::multimap<QgsPointXY, QgsFeatureId, PointComparer>::iterator pointIt = endVerticesMap.begin(), end = endVerticesMap.end(); pointIt != end; pointIt = endVerticesMap.upper_bound( pointIt->first ) )
  {
    QgsPointXY p = pointIt->first;
    QgsFeatureId k = pointIt->second;

    size_t repetitions = endVerticesMap.count( p );

    if ( repetitions == 2 )
    {
      QgsGeometry conflictGeom = QgsGeometry::fromPointXY( p );

      if ( isExtent )
      {
        if ( canvasExtentPoly.disjoint( conflictGeom ) )
        {
          continue;
        }
      }

      QgsRectangle bBox = conflictGeom.boundingBox();
      QgsFeature feat;

      FeatureLayer ftrLayer1;
      //need to fetch attributes?? being safe side by fetching..
      layer1->getFeatures( QgsFeatureRequest().setFilterFid( k ) ).nextFeature( feat );
      ftrLayer1.feature = feat;
      ftrLayer1.layer = layer1;

      QList<FeatureLayer> errorFtrLayers;
      errorFtrLayers << ftrLayer1 << ftrLayer1;

      TopolErrorPseudos *err = new TopolErrorPseudos( bBox, conflictGeom, errorFtrLayers );
      errorList << err;

    }
  }
  return errorList;
}

ErrorList topolTest::checkValid( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  Q_UNUSED( layer1 );
  Q_UNUSED( layer2 );
  Q_UNUSED( isExtent );

  int i = 0;
  ErrorList errorList;
  QgsFeature f;

  QList<FeatureLayer>::Iterator it;

  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( ++i );
    if ( testCanceled() )
      break;

    QgsGeometry g = it->feature.geometry();
    if ( g.isNull() )
    {
      QgsMessageLog::logMessage( tr( "Invalid geometry in validity test." ), tr( "Topology plugin" ) );
      continue;
    }

    GEOSGeometry *gGeos = g.exportToGeos();
    if ( !gGeos )
      continue;

    if ( !GEOSisValid_r( QgsGeometry::getGEOSHandler(), gGeos ) )
    {
      QgsRectangle r = g.boundingBox();
      QList<FeatureLayer> fls;
      fls << *it << *it;

      TopolErrorValid *err = new TopolErrorValid( r, g, fls );
      errorList << err;
    }
    GEOSGeom_destroy_r( QgsGeometry::getGEOSHandler(), gGeos );
  }

  return errorList;
}


ErrorList topolTest::checkPointCoveredBySegment( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  int i = 0;

  ErrorList errorList;

  if ( layer1->geometryType() != QgsWkbTypes::PointGeometry )
  {
    return errorList;
  }
  if ( layer2->geometryType() == QgsWkbTypes::PointGeometry )
  {
    return errorList;
  }

  QgsSpatialIndex *index = mLayerIndexes[layer2->id()];
  QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );

  QList<FeatureLayer>::Iterator it;
  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );

    if ( testCanceled() )
      break;

    QgsGeometry g1 = it->feature.geometry();
    QgsRectangle bb = g1.boundingBox();

    QList<QgsFeatureId> crossingIds;
    crossingIds = index->intersects( bb );

    QList<QgsFeatureId>::ConstIterator cit = crossingIds.begin();
    QList<QgsFeatureId>::ConstIterator crossingIdsEnd = crossingIds.end();

    bool touched = false;

    for ( ; cit != crossingIdsEnd; ++cit )
    {
      QgsFeature &f = mFeatureMap2[*cit].feature;
      QgsGeometry g2 = f.geometry();

      if ( g2.isNull() )
      {
        QgsMessageLog::logMessage( tr( "Invalid geometry in covering test." ), tr( "Topology plugin" ) );
        continue;
      }

      // test if point touches other geometry
      if ( g1.touches( g2 ) )
      {
        touched = true;
        break;
      }
    }

    if ( !touched )
    {
      QgsGeometry conflictGeom = QgsGeometry( g1 );

      if ( isExtent )
      {
        if ( canvasExtentPoly.disjoint( conflictGeom ) )
        {
          continue;
        }
      }

      QList<FeatureLayer> fls;
      fls << *it << *it;
      //bb.scale(10);

      TopolErrorCovered *err = new TopolErrorCovered( bb, conflictGeom, fls );

      errorList << err;
    }
  }
  return errorList;
}

ErrorList topolTest::checkOverlapWithLayer( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  int i = 0;
  ErrorList errorList;

  bool skipItself = layer1 == layer2;
  QgsSpatialIndex *index = mLayerIndexes[layer2->id()];

  QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );

  QList<FeatureLayer>::iterator it;
  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );

    if ( testCanceled() )
      break;

    QgsGeometry g1 = it->feature.geometry();
    QgsRectangle bb = g1.boundingBox();

    QList<QgsFeatureId> crossingIds;
    crossingIds = index->intersects( bb );

    QList<QgsFeatureId>::ConstIterator cit = crossingIds.begin();
    QList<QgsFeatureId>::ConstIterator crossingIdsEnd = crossingIds.end();
    for ( ; cit != crossingIdsEnd; ++cit )
    {
      QgsFeature &f = mFeatureMap2[*cit].feature;
      QgsGeometry g2 = f.geometry();

      // skip itself, when invoked with the same layer
      if ( skipItself && f.id() == it->feature.id() )
        continue;

      if ( g2.isNull() )
      {
        QgsMessageLog::logMessage( tr( "Second geometry missing." ), tr( "Topology plugin" ) );
        continue;
      }

      if ( g1.overlaps( g2 ) )
      {
        QgsRectangle r = bb;
        QgsRectangle r2 = g2.boundingBox();
        r.combineExtentWith( r2 );

        QgsGeometry conflictGeom = g1.intersection( g2 );
        // could this for some reason return NULL?
        if ( conflictGeom.isNull() )
        {
          continue;
        }

        if ( isExtent )
        {
          if ( canvasExtentPoly.disjoint( conflictGeom ) )
          {
            continue;
          }
          if ( canvasExtentPoly.crosses( conflictGeom ) )
          {
            conflictGeom = conflictGeom.intersection( canvasExtentPoly );
          }
        }

        //c = new QgsGeometry;

        QList<FeatureLayer> fls;
        FeatureLayer fl;
        fl.feature = f;
        fl.layer = layer2;
        fls << *it << fl;
        TopolErrorIntersection *err = new TopolErrorIntersection( r, conflictGeom, fls );

        errorList << err;
      }
    }
  }
  return errorList;
}



ErrorList topolTest::checkPointCoveredByLineEnds( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  int i = 0;
  ErrorList errorList;


  if ( layer1->geometryType() != QgsWkbTypes::PointGeometry )
  {
    return errorList;
  }

  if ( layer2->geometryType() != QgsWkbTypes::LineGeometry )
  {
    return errorList;
  }

  QgsSpatialIndex *index = mLayerIndexes[layer2->id()];
  QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );

  QList<FeatureLayer>::Iterator it;
  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );
    if ( testCanceled() )
      break;
    QgsGeometry g1 = it->feature.geometry();
    QgsRectangle bb = g1.boundingBox();
    QList<QgsFeatureId> crossingIds;
    crossingIds = index->intersects( bb );
    QList<QgsFeatureId>::ConstIterator cit = crossingIds.constBegin();
    QList<QgsFeatureId>::ConstIterator crossingIdsEnd = crossingIds.constEnd();
    bool touched = false;
    for ( ; cit != crossingIdsEnd; ++cit )
    {
      QgsFeature &f = mFeatureMap2[*cit].feature;
      QgsGeometry g2 = f.geometry();
      if ( g2.isNull() || !_canExportToGeos( g2 ) )
      {
        QgsMessageLog::logMessage( tr( "Second geometry missing or GEOS import failed." ), tr( "Topology plugin" ) );
        continue;
      }
      QgsPolylineXY g2Line = g2.asPolyline();
      QgsGeometry startPoint = QgsGeometry::fromPointXY( g2Line.at( 0 ) );
      QgsGeometry endPoint = QgsGeometry::fromPointXY( g2Line.last() );
      touched = g1.intersects( startPoint ) || g1.intersects( endPoint );

      if ( touched )
      {
        break;
      }
    }
    if ( !touched )
    {
      QgsGeometry conflictGeom = g1;
      if ( isExtent )
      {
        if ( canvasExtentPoly.disjoint( conflictGeom ) )
        {
          continue;
        }
      }

      QList<FeatureLayer> fls;
      fls << *it << *it;
      //bb.scale(10);

      TopolErrorPointNotCoveredByLineEnds *err = new TopolErrorPointNotCoveredByLineEnds( bb, conflictGeom, fls );
      errorList << err;
    }
  }
  return errorList;
}

ErrorList topolTest::checkyLineEndsCoveredByPoints( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  int i = 0;
  ErrorList errorList;


  if ( layer1->geometryType() != QgsWkbTypes::LineGeometry )
  {
    return errorList;
  }

  if ( layer2->geometryType() != QgsWkbTypes::PointGeometry )
  {
    return errorList;
  }

  QgsSpatialIndex *index = mLayerIndexes[layer2->id()];

  QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );

  QList<FeatureLayer>::Iterator it;
  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );
    if ( testCanceled() )
      break;
    QgsGeometry g1 = it->feature.geometry();

    QgsPolylineXY g1Polyline = g1.asPolyline();
    QgsGeometry startPoint = QgsGeometry::fromPointXY( g1Polyline.at( 0 ) );
    QgsGeometry endPoint = QgsGeometry::fromPointXY( g1Polyline.last() );

    QgsRectangle bb = g1.boundingBox();
    QList<QgsFeatureId> crossingIds;
    crossingIds = index->intersects( bb );
    QList<QgsFeatureId>::ConstIterator cit = crossingIds.begin();
    QList<QgsFeatureId>::ConstIterator crossingIdsEnd = crossingIds.end();
    bool touched = false;

    bool touchStartPoint = false;
    bool touchEndPoint = false;

    for ( ; cit != crossingIdsEnd; ++cit )
    {
      QgsFeature &f = mFeatureMap2[*cit].feature;
      QgsGeometry g2 = f.geometry();
      if ( g2.isNull() || !_canExportToGeos( g2 ) )
      {
        QgsMessageLog::logMessage( tr( "Second geometry missing or GEOS import failed." ), tr( "Topology plugin" ) );
        continue;
      }


      if ( g2.intersects( startPoint ) )
      {
        touchStartPoint = true;
      }

      if ( g2.intersects( endPoint ) )
      {
        touchEndPoint = true;
      }

      if ( touchStartPoint && touchEndPoint )
      {
        touched = true;
        break;
      }

    }

    if ( !touched )
    {
      QgsGeometry conflictGeom = g1;

      if ( isExtent )
      {
        if ( canvasExtentPoly.disjoint( conflictGeom ) )
        {
          continue;
        }
        if ( canvasExtentPoly.crosses( conflictGeom ) )
        {
          conflictGeom = conflictGeom.intersection( canvasExtentPoly );
        }
      }
      QList<FeatureLayer> fls;
      fls << *it << *it;
      //bb.scale(10);

      TopolErrorLineEndsNotCoveredByPoints *err = new TopolErrorLineEndsNotCoveredByPoints( bb, conflictGeom, fls );
      errorList << err;
    }
  }
  return errorList;
}

ErrorList topolTest::checkPointInPolygon( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  int i = 0;
  ErrorList errorList;

  if ( layer1->geometryType() != QgsWkbTypes::PointGeometry )
  {
    return errorList;
  }

  if ( layer2->geometryType() != QgsWkbTypes::PolygonGeometry )
  {
    return errorList;
  }

  QgsSpatialIndex *index = mLayerIndexes[layer2->id()];

  QgsGeometry canvasExtentPoly = QgsGeometry::fromWkt( qgsInterface->mapCanvas()->extent().asWktPolygon() );

  QList<FeatureLayer>::Iterator it;
  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );
    if ( testCanceled() )
      break;
    QgsGeometry g1 = it->feature.geometry();
    QgsRectangle bb = g1.boundingBox();
    QList<QgsFeatureId> crossingIds;
    crossingIds = index->intersects( bb );
    QList<QgsFeatureId>::ConstIterator cit = crossingIds.begin();
    QList<QgsFeatureId>::ConstIterator crossingIdsEnd = crossingIds.end();
    bool touched = false;
    for ( ; cit != crossingIdsEnd; ++cit )
    {
      QgsFeature &f = mFeatureMap2[*cit].feature;
      QgsGeometry g2 = f.geometry();
      if ( g2.isNull() || !_canExportToGeos( g2 ) )
      {
        QgsMessageLog::logMessage( tr( "Second geometry missing or GEOS import failed." ), tr( "Topology plugin" ) );
        continue;
      }
      if ( g2.contains( g1 ) )
      {
        touched = true;
        break;
      }
    }
    if ( !touched )
    {
      QgsGeometry conflictGeom = g1;

      if ( isExtent )
      {
        if ( canvasExtentPoly.disjoint( conflictGeom ) )
        {
          continue;
        }
      }

      QList<FeatureLayer> fls;
      fls << *it << *it;
      //bb.scale(10);

      TopolErrorPointNotInPolygon *err = new TopolErrorPointNotInPolygon( bb, conflictGeom, fls );
      errorList << err;
    }
  }

  return errorList;
}


ErrorList topolTest::checkPolygonContainsPoint( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  Q_UNUSED( isExtent );

  int i = 0;
  ErrorList errorList;

  if ( layer1->geometryType() != QgsWkbTypes::PolygonGeometry )
  {
    return errorList;
  }

  if ( layer2->geometryType() != QgsWkbTypes::PointGeometry )
  {
    return errorList;
  }

  QgsSpatialIndex *index = mLayerIndexes[layer2->id()];

  QList<FeatureLayer>::Iterator it;
  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );
    if ( testCanceled() )
      break;
    QgsGeometry g1 = it->feature.geometry();
    QgsRectangle bb = g1.boundingBox();
    QList<QgsFeatureId> crossingIds;
    crossingIds = index->intersects( bb );
    QList<QgsFeatureId>::ConstIterator cit = crossingIds.begin();
    QList<QgsFeatureId>::ConstIterator crossingIdsEnd = crossingIds.constEnd();
    bool touched = false;
    for ( ; cit != crossingIdsEnd; ++cit )
    {
      QgsFeature &f = mFeatureMap2[*cit].feature;
      QgsGeometry g2 = f.geometry();
      if ( g2.isNull() || !_canExportToGeos( g2 ) )
      {
        QgsMessageLog::logMessage( tr( "Second geometry missing or GEOS import failed." ), tr( "Topology plugin" ) );
        continue;
      }
      if ( g1.contains( g2 ) )
      {
        touched = true;
        break;
      }
    }
    if ( !touched )
    {
      QList<FeatureLayer> fls;
      fls << *it << *it;
      //bb.scale(10);
      TopolErrorPolygonContainsPoint *err = new TopolErrorPolygonContainsPoint( bb, g1, fls );
      errorList << err;
    }
  }
  return errorList;
}

ErrorList topolTest::checkMultipart( QgsVectorLayer *layer1, QgsVectorLayer *layer2, bool isExtent )
{
  Q_UNUSED( layer2 );
  Q_UNUSED( layer1 );
  Q_UNUSED( isExtent );

  int i = 0;
  ErrorList errorList;
  QList<FeatureLayer>::Iterator it;
  for ( it = mFeatureList1.begin(); it != mFeatureList1.end(); ++it )
  {
    if ( !( ++i % 100 ) )
      emit progress( ++i );
    if ( testCanceled() )
      break;
    QgsGeometry g = it->feature.geometry();
    if ( g.isNull() )
    {
      QgsMessageLog::logMessage( tr( "Missing geometry in multipart check." ), tr( "Topology plugin" ) );
      continue;
    }
    if ( !_canExportToGeos( g ) )
      continue;
    if ( g.isMultipart() )
    {
      QgsRectangle r = g.boundingBox();
      QList<FeatureLayer> fls;
      fls << *it << *it;
      TopolErroMultiPart *err = new TopolErroMultiPart( r, g, fls );
      errorList << err;
    }
  }
  return errorList;
}

void topolTest::fillFeatureMap( QgsVectorLayer *layer, const QgsRectangle &extent )
{
  QgsFeatureIterator fit;
  if ( extent.isEmpty() )
  {
    fit = layer->getFeatures( QgsFeatureRequest().setSubsetOfAttributes( QgsAttributeList() ) );
  }
  else
  {
    fit = layer->getFeatures( QgsFeatureRequest()
                              .setFilterRect( extent )
                              .setFlags( QgsFeatureRequest::ExactIntersect )
                              .setSubsetOfAttributes( QgsAttributeList() ) );
  }

  QgsFeature f;

  while ( fit.nextFeature( f ) )
  {
    if ( f.hasGeometry() )
    {
      mFeatureMap2[f.id()] = FeatureLayer( layer, f );
    }
  }
}

void topolTest::fillFeatureList( QgsVectorLayer *layer, const QgsRectangle &extent )
{
  QgsFeatureIterator fit;
  if ( extent.isEmpty() )
  {
    fit = layer->getFeatures( QgsFeatureRequest().setSubsetOfAttributes( QgsAttributeList() ) );
  }
  else
  {
    fit = layer->getFeatures( QgsFeatureRequest()
                              .setFilterRect( extent )
                              .setFlags( QgsFeatureRequest::ExactIntersect )
                              .setSubsetOfAttributes( QgsAttributeList() ) );
  }

  QgsFeature f;

  while ( fit.nextFeature( f ) )
  {
    if ( f.hasGeometry() )
    {
      mFeatureList1 << FeatureLayer( layer, f );
    }
  }

}

QgsSpatialIndex *topolTest::createIndex( QgsVectorLayer *layer, const QgsRectangle &extent )
{
  QgsSpatialIndex *index = new QgsSpatialIndex();

  QgsFeatureIterator fit;
  if ( extent.isEmpty() )
  {
    fit = layer->getFeatures( QgsFeatureRequest().setSubsetOfAttributes( QgsAttributeList() ) );
  }
  else
  {
    fit = layer->getFeatures( QgsFeatureRequest()
                              .setFilterRect( extent )
                              .setFlags( QgsFeatureRequest::ExactIntersect )
                              .setSubsetOfAttributes( QgsAttributeList() ) );
  }


  int i = 0;
  QgsFeature f;
  while ( fit.nextFeature( f ) )
  {
    if ( !( ++i % 100 ) )
      emit progress( i );

    if ( testCanceled() )
    {
      delete index;
      return nullptr;
    }

    if ( f.hasGeometry() )
    {
      index->insertFeature( f );
      mFeatureMap2[f.id()] = FeatureLayer( layer, f );
    }
  }

  return index;
}

ErrorList topolTest::runTest( const QString &testName, QgsVectorLayer *layer1, QgsVectorLayer *layer2, ValidateType type )
{
  QgsDebugMsg( QString( "Running test %1" ).arg( testName ) );
  ErrorList errors;

  if ( !layer1 )
  {
    QgsMessageLog::logMessage( tr( "First layer not found in registry." ), tr( "Topology plugin" ) );
    return errors;
  }

  if ( !layer2 && mTopologyRuleMap[testName].useSecondLayer )
  {
    QgsMessageLog::logMessage( tr( "Second layer not found in registry." ), tr( "Topology plugin" ) );
    return errors;
  }

  mFeatureList1.clear();
  mFeatureMap2.clear();

  //checking if new features are not
  //being recognised due to indexing not being up to date

  mLayerIndexes.clear();

  if ( mTopologyRuleMap[testName].useSecondLayer )
  {
    // validate all features or current extent
    QgsRectangle extent;
    if ( type == ValidateExtent )
    {
      extent = qgsInterface->mapCanvas()->extent();
    }
    else
    {
      extent = QgsRectangle();
    }

    fillFeatureList( layer1, extent );
    //fillFeatureMap( layer1, extent );

    if ( !mLayerIndexes.contains( layer2->id() ) )
    {
      mLayerIndexes[layer2->id()] = createIndex( layer2, extent );
    }
  }
  else
  {
    // validate all features or current extent
    QgsRectangle extent;
    if ( type == ValidateExtent )
    {
      extent = qgsInterface->mapCanvas()->extent();
      if ( mTopologyRuleMap[testName].useSpatialIndex )
      {
        mLayerIndexes[layer1->id()] = createIndex( layer1, qgsInterface->mapCanvas()->extent() );
      }
      else
      {
        fillFeatureList( layer1, extent );
      }
    }
    else
    {
      if ( mTopologyRuleMap[testName].useSpatialIndex )
      {
        if ( !mLayerIndexes.contains( layer1->id() ) )
        {

          mLayerIndexes[layer1->id()] = createIndex( layer1, QgsRectangle() );
        }
      }
      else
      {
        fillFeatureList( layer1, QgsRectangle() );
      }
    }

  }

  //call test routine
  bool isValidatingExtent;
  isValidatingExtent = type == ValidateExtent;

  return ( this->*( mTopologyRuleMap[testName].f ) )( layer1, layer2, isValidatingExtent );
}
