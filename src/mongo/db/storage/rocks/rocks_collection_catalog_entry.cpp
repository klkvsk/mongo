// rocks_collection_catalog_entry.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/rocks/rocks_collection_catalog_entry.h"

#include <rocksdb/db.h>

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/rocks/rocks_engine.h"

namespace mongo {

    /**
     * bson schema
     * { ns: <name for sanity>,
     *   indexes : [ { spec : <bson spec>,
     *                 ready: <bool>,
     *                 head: DiskLoc,
     *                 multikey: <bool> } ]
     *  }
     */

    RocksCollectionCatalogEntry::RocksCollectionCatalogEntry( RocksEngine* engine,
                                                              const StringData& ns )
        : CollectionCatalogEntry( ns ), _engine( engine ) {
        _metaDataKey = string("metadata-") + ns.toString();
    }

    // ------- indexes ----------

    int RocksCollectionCatalogEntry::getTotalIndexCount() const {
        MetaData md;
        _getMetaData( &md );

        return static_cast<int>( md.indexes.size() );
    }

    int RocksCollectionCatalogEntry::getCompletedIndexCount() const {
        MetaData md;
        _getMetaData( &md );

        int num = 0;
        for ( unsigned i = 0; i < md.indexes.size(); i++ ) {
            if ( md.indexes[i].ready )
                num++;
        }
        return num;
    }

    int RocksCollectionCatalogEntry::getMaxAllowedIndexes() const {
        return 64; // for compatability for now, could be higher
    }

    void RocksCollectionCatalogEntry::getAllIndexes( std::vector<std::string>* names ) const {
        MetaData md;
        _getMetaData( &md );

        for ( unsigned i = 0; i < md.indexes.size(); i++ ) {
            names->push_back( md.indexes[i].spec["name"].String() );
        }
    }

    BSONObj RocksCollectionCatalogEntry::getIndexSpec( const StringData& indexName ) const {
        MetaData md;
        _getMetaData( &md );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        return md.indexes[offset].spec.getOwned();
    }

    bool RocksCollectionCatalogEntry::isIndexMultikey( const StringData& indexName) const {
        MetaData md;
        _getMetaData( &md );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        return md.indexes[offset].multikey;
    }

    DiskLoc RocksCollectionCatalogEntry::getIndexHead( const StringData& indexName ) const {
        MetaData md;
        _getMetaData( &md );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        return md.indexes[offset].head;
    }

    bool RocksCollectionCatalogEntry::isIndexReady( const StringData& indexName ) const {
        MetaData md;
        _getMetaData( &md );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        return md.indexes[offset].ready;
    }

    bool RocksCollectionCatalogEntry::setIndexIsMultikey(OperationContext* txn,
                                                         const StringData& indexName,
                                                         bool multikey ) {
        boost::mutex::scoped_lock lk( _metaDataLock );
        MetaData md;
        _getMetaData_inlock( &md );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        if ( md.indexes[offset].multikey == multikey )
            return false;
        md.indexes[offset].multikey = multikey;
        _putMetaData_inlock( md );
        return true;
    }

    void RocksCollectionCatalogEntry::setIndexHead( OperationContext* txn,
                                                    const StringData& indexName,
                                                    const DiskLoc& newHead ) {
        boost::mutex::scoped_lock lk( _metaDataLock );
        MetaData md;
        _getMetaData_inlock( &md );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        md.indexes[offset].head = newHead;
        _putMetaData_inlock( md );
    }

    void RocksCollectionCatalogEntry::indexBuildSuccess( OperationContext* txn,
                                                         const StringData& indexName ) {
        boost::mutex::scoped_lock lk( _metaDataLock );
        MetaData md;
        _getMetaData_inlock( &md );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        md.indexes[offset].ready = true;
        _putMetaData_inlock( md );
    }


    Status RocksCollectionCatalogEntry::removeIndex( OperationContext* txn,
                                                     const StringData& indexName ) {
        // remove column faily from Engine
        // remove info from meta data
        invariant( !"can't remove index in rocks yet" );
    }

    Status RocksCollectionCatalogEntry::prepareForIndexBuild( OperationContext* txn,
                                                              const IndexDescriptor* spec ) {
        boost::mutex::scoped_lock lk( _metaDataLock );
        MetaData md;
        _getMetaData_inlock( &md );
        md.indexes.push_back( IndexMetaData( spec->infoObj(), false, DiskLoc(), false ) );
        _putMetaData_inlock( md );
        return Status::OK();
    }

    /* Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
     * The specified index must already contain an expireAfterSeconds field, and the value in
     * that field and newExpireSecs must both be numeric.
     */
    void RocksCollectionCatalogEntry::updateTTLSetting( OperationContext* txn,
                                                        const StringData& indexName,
                                                        long long newExpireSeconds ) {
        invariant( !"ttl settings change not supported in rocks yet" );
    }

    void RocksCollectionCatalogEntry::createMetaData() {
        string result;
        rocksdb::Status status = _engine->getDB()->Get( rocksdb::ReadOptions(),
                                                        _metaDataKey,
                                                        &result );
        invariant( status.IsNotFound() );

        MetaData md;
        md.ns = ns();

        BSONObj obj = md.toBSON();
        status = _engine->getDB()->Put( rocksdb::WriteOptions(),
                                        _metaDataKey,
                                        rocksdb::Slice( obj.objdata(),
                                                        obj.objsize() ) );
        invariant( status.ok() );
    }


    void RocksCollectionCatalogEntry::dropMetaData() {
        rocksdb::Status status = _engine->getDB()->Delete( rocksdb::WriteOptions(), _metaDataKey );
        invariant( status.ok() );
    }

    bool RocksCollectionCatalogEntry::_getMetaData( MetaData* out ) const {
        boost::mutex::scoped_lock lk( _metaDataLock );
        return _getMetaData_inlock( out );
    }

    bool RocksCollectionCatalogEntry::_getMetaData_inlock( MetaData* out ) const {
        string result;
        rocksdb::Status status = _engine->getDB()->Get( rocksdb::ReadOptions(),
                                                        _metaDataKey,
                                                        &result );
        invariant( !status.IsNotFound() );
        invariant( status.ok() );
        BSONObj obj( result.c_str() );
        out->parse( obj );
        return true;
    }

    void RocksCollectionCatalogEntry::_putMetaData_inlock( const MetaData& in ) {
        // XXX: this should probably be done via the RocksRecoveryUnit.
        BSONObj obj = in.toBSON();
        rocksdb::Status status = _engine->getDB()->Put( rocksdb::WriteOptions(),
                                                        _metaDataKey,
                                                        rocksdb::Slice( obj.objdata(),
                                                                        obj.objsize() ) );
        invariant( status.ok() );
    }

    int RocksCollectionCatalogEntry::MetaData::findIndexOffset( const StringData& name ) const {
        for ( unsigned i = 0; i < indexes.size(); i++ )
            if ( indexes[i].spec["name"].String() == name )
                return i;
        return -1;
    }

    BSONObj RocksCollectionCatalogEntry::MetaData::toBSON() const {
        BSONObjBuilder b;
        b.append( "ns", ns );
        {
            BSONArrayBuilder arr( b.subarrayStart( "indexes" ) );
            for ( unsigned i = 0; i < indexes.size(); i++ ) {
                BSONObjBuilder sub( arr.subobjStart() );
                sub.append( "spec", indexes[i].spec );
                sub.appendBool( "ready", indexes[i].ready );
                sub.appendBool( "multikey", indexes[i].multikey );
                sub.append( "head_a", indexes[i].head.a() );
                sub.append( "head_b", indexes[i].head.getOfs() );
                sub.done();
            }
            arr.done();
        }
        return b.obj();
    }

    void RocksCollectionCatalogEntry::MetaData::parse( const BSONObj& obj ) {
        ns = obj["ns"].valuestrsafe();

        BSONElement e = obj["indexes"];
        if ( e.isABSONObj() ) {
            std::vector<BSONElement> entries = e.Array();
            for ( unsigned i = 0; i < entries.size(); i++ ) {
                BSONObj idx = entries[i].Obj();
                IndexMetaData imd;
                imd.spec = idx["spec"].Obj();
                imd.ready = idx["ready"].trueValue();
                imd.head = DiskLoc( idx["head_a"].Int(),
                                    idx["head_b"].Int() );
                imd.multikey = idx["multikey"].trueValue();
                indexes.push_back( imd );
            }
        }
    }
}
