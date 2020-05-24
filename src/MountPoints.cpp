/*
 *   File name: MountPoints.cpp
 *   Summary:	Support classes for QDirStat
 *   License:	GPL V2 - See file LICENSE for details.
 *
 *   Author:	Stefan Hundhammer <Stefan.Hundhammer@gmx.de>
 */


#include <QFile>
#include <QRegExp>
#include <QFileInfo>

#include "MountPoints.h"
#include "Logger.h"
#include "Exception.h"

using namespace QDirStat;


MountPoint::MountPoint( const QString & device,
                        const QString & path,
                        const QString & filesystemType,
                        const QString & mountOptions ) :
    _device( device ),
    _path( path ),
    _filesystemType( filesystemType ),
    _isDuplicate( false )
{
    _mountOptions = mountOptions.split( "," );
}


QString MountPoint::mountOptionsStr() const
{
    return _mountOptions.join( "," );
}


bool MountPoint::isReadOnly() const
{
    return _mountOptions.contains( "ro" );
}


bool MountPoint::isBtrfs() const
{
    return _filesystemType.toLower() == "btrfs";
}


bool MountPoint::isNetworkMount() const
{
    QString fsType = _filesystemType.toLower();

    if ( fsType.startsWith( "nfs"  ) ) return true;
    if ( fsType.startsWith( "cifs" ) ) return true;

    return false;
}


bool MountPoint::isSystemMount() const
{
    // All normal block have a path with a slash like "/dev/something" or on some
    // systems maybe "/devices/something". NFS mounts have "hostname:/some/path",
    // Samba mounts have "//hostname/some/path".
    //
    // This check filters out system devices like "cgroup", "tmpfs", "sysfs"
    // and all those other kernel-table devices.

    if ( ! _device.contains( "/" ) )    return true;

    if ( _path.startsWith( "/dev"  ) )  return true;
    if ( _path.startsWith( "/proc" ) )  return true;
    if ( _path.startsWith( "/sys"  ) )  return true;

    return false;
}




MountPoints * MountPoints::_instance = 0;


MountPoints * MountPoints::instance()
{
    if ( ! _instance )
    {
        _instance = new MountPoints();
        CHECK_NEW( _instance );
    }

    return _instance;
}


MountPoints::MountPoints()
{
    init();
}


MountPoints::~MountPoints()
{
    init();
}


void MountPoints::init()
{
    qDeleteAll( _mountPointList );
    _mountPointList.clear();
    _mountPointMap.clear();
    _isPopulated     = false;
    _hasBtrfs        = false;
    _checkedForBtrfs = false;
}


void MountPoints::clear()
{
    if ( _instance )
        _instance->init();
}


bool MountPoints::isEmpty()
{
    instance()->ensurePopulated();

    return instance()->_mountPointList.isEmpty();
}


const MountPoint * MountPoints::findByPath( const QString & path )
{
    instance()->ensurePopulated();

    return instance()->_mountPointMap.value( path, 0 );
}


const MountPoint * MountPoints::findNearestMountPoint( const QString & startPath )
{
    QFileInfo fileInfo( startPath );
    QString path = fileInfo.canonicalFilePath(); // absolute path without symlinks or ..

    if ( path != startPath )
        logDebug() << startPath << " canonicalized is " << path << endl;

    const MountPoint * mountPoint = findByPath( path );

    if ( ! mountPoint )
    {
        QStringList pathComponents = startPath.split( "/", QString::SkipEmptyParts );

        while ( ! mountPoint && ! pathComponents.isEmpty() )
        {
            // Try one level upwards
            pathComponents.removeLast();
            path = QString( "/" ) + pathComponents.join( "/" );

            mountPoint = instance()->_mountPointMap.value( path, 0 );
        }
    }

    // logDebug() << "Nearest mount point for " << startPath << " is " << mountPoint << endl;

    return mountPoint;
}


bool MountPoints::isDeviceMounted( const QString & device )
{
    // Do NOT call ensurePopulated() here: This would cause a recursion in the
    // populating process!

    foreach ( const MountPoint * mountPoint, instance()->_mountPointList )
    {
        if ( mountPoint->device() == device )
            return true;
    }

    return false;
}


bool MountPoints::hasBtrfs()
{
    instance()->ensurePopulated();

    if ( ! _instance->_checkedForBtrfs )
    {
        _instance->_hasBtrfs = _instance->checkForBtrfs();
        _instance->_checkedForBtrfs = true;
    }

    return _instance->_hasBtrfs;
}


void MountPoints::ensurePopulated()
{
    if ( _isPopulated )
        return;

    read( "/proc/mounts" ) || read( "/etc/mtab" );

    if ( ! _isPopulated )
        logError() << "Could not read either /proc/mounts or /etc/mtab" << endl;

    _isPopulated = true;
    // dumpNormalMountPoints();
}


bool MountPoints::read( const QString & filename )
{
    QFile file( filename );

    if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )
    {
        logWarning() << "Can't open " << filename << endl;
        return false;
    }

    QTextStream in( &file );
    int lineNo = 0;
    int count  = 0;
    QString line = in.readLine();

    while ( ! line.isNull() ) // in.atEnd() always returns true for /proc/*
    {
        ++lineNo;
        QStringList fields = line.split( QRegExp( "\\s+" ), QString::SkipEmptyParts );

        if ( fields.isEmpty() ) // allow empty lines
            continue;

        if ( fields.size() < 4 )
        {
            logError() << "Bad line " << filename << ":" << lineNo << ": " << line << endl;
            continue;
        }

        // File format (/proc/mounts or /etc/mtab):
        //
        //   /dev/sda6 / ext4 rw,relatime,errors=remount-ro,data=ordered 0 0
        //   /dev/sda7 /work ext4 rw,relatime,data=ordered 0 0
        //   nas:/share/work /nas/work nfs rw,local_lock=none 0 0

        QString device    = fields[0];
        QString path      = fields[1];
        QString fsType    = fields[2];
        QString mountOpts = fields[3];
        // ignoring fsck and dump order (0 0)

        MountPoint * mountPoint = new MountPoint( device, path, fsType, mountOpts );
        CHECK_NEW( mountPoint );

        if ( ( ! mountPoint->isSystemMount() ) && isDeviceMounted( device ) )
        {
            mountPoint->setDuplicate();
            logInfo() << "Found duplicate mount of " << device << " at " << path << endl;
        }

        _mountPointList << mountPoint;
        _mountPointMap[ path ] = mountPoint;
        ++count;

        line = in.readLine();
    }

    if ( count < 1 )
    {
        logWarning() << "Not a single mount point in " << filename << endl;
        return false;
    }
    else
    {
        _isPopulated = true;
        return true;
    }
}


bool MountPoints::checkForBtrfs()
{
    ensurePopulated();

    foreach ( const MountPoint * mountPoint, _mountPointMap )
    {
        if ( mountPoint && mountPoint->isBtrfs() )
            return true;
    }

    return false;
}


QList<const MountPoint *> MountPoints::normalMountPoints()
{
    QList<const MountPoint *> result;

    foreach ( const MountPoint * mountPoint, instance()->_mountPointList )
    {
        if ( ! mountPoint->isSystemMount() && ! mountPoint->isDuplicate() )
            result << mountPoint;
    }

    return result;
}


void MountPoints::dumpNormalMountPoints()
{
    foreach ( const MountPoint * mountPoint, normalMountPoints() )
        logDebug() << mountPoint << endl;
}


void MountPoints::dump()
{
    foreach ( const MountPoint * mountPoint, instance()->_mountPointList )
    {
        logDebug() << mountPoint << endl;
    }
}
