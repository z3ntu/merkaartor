/***************************************************************************
 *   Copyright (C) 2005 by Robin Gingras                                   *
 *   neozenkai@cox.net                                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <QThread>
#include <QObject>
#include <QString>
#include <QMutex>
#include <QFile>
#include <QStringList>
#include <QMessageBox>
#include <QTcpSocket>
#include <QTimer>
#include <QHostAddress>

#include "qgpsdevice.h"
#ifndef _MOBILE
#include "qextserialport.h"
#endif
#include <math.h>

#ifdef USE_GPSD_LIB
    #include <cerrno>
    #include <cstring> // strerror()
#endif

#include "MerkaartorPreferences.h"

/* GPSSLOTFORWARDER */

GPSSlotForwarder::GPSSlotForwarder(QGPSDevice* aTarget)
: Target(aTarget)
{
}

void GPSSlotForwarder::onLinkReady()
{
    Target->onLinkReady();
}

void GPSSlotForwarder::onDataAvailable()
{
    Target->onDataAvailable();
}

void GPSSlotForwarder::onStop()
{
    Target->onStop();
}

void GPSSlotForwarder::checkDataAvailable()
{
    Target->checkDataAvailable();
}

/**
 * QGPSDevice::QGPSDevice()
 *
 * Takes in an optional serialPort string and sets the serialPort property
 * accordingly.
 *
 * @param char serialPort   Serial port to listen to for GPS dat
 */

QGPSDevice::QGPSDevice()
    :LogFile(0)
{
    mutex = new QMutex(QMutex::Recursive);

    setLatitude(0);
    setLongitude(0);
    setAltitude(0);
    setHeading(0);
    setSpeed(0);
    setVariation(0);

    setFixMode(QGPSDevice::FixAuto);
    setFixType(QGPSDevice::FixUnavailable);
    setFixStatus(QGPSDevice::StatusVoid);

    stopLoop = false;

    cur_numSatellites = 0;
    for(int i = 0; i < 50; i ++)
    {
        satArray[i][0] = satArray[i][1] = satArray[i][2] = 0;
    }
}

/**
 * Accessor functions
 */

int QGPSDevice::latDegrees()    { return (int) (fabs(latitude())); }
int QGPSDevice::latMinutes()
{
    qreal m = fabs(latitude()) - latDegrees();
    return int(m * 60);
}
int QGPSDevice::latSeconds()
{
    qreal m = fabs(latitude()) - latDegrees();
    qreal s = (m * 60) - int(m * 60);
    return int(s * 60);
}
int QGPSDevice::longDegrees()    { return (int) (fabs(longitude())); }
int QGPSDevice::longMinutes()
{
    qreal m = fabs(longitude()) - longDegrees();
    return int(m * 60);
}
int QGPSDevice::longSeconds()
{
    qreal m = fabs(longitude()) - longDegrees();
    qreal s = (m * 60) - int(m * 60);
    return int(s * 60);
}

bool QGPSDevice::isActiveSat(int prn)
{
    for (int i=0; i<12; i++) {
        if (activeSats[i] == prn)
            return true;
    }
    return false;
}

void QGPSDevice::satInfo(int index, int &elev, int &azim, int &snr)
{
    mutex->lock();

    elev = satArray[index][0];
    azim = satArray[index][1];
    snr  = satArray[index][2];

    mutex->unlock();
}

/**
 * QGPSDevice::run()
 *
 * Begins the thread loop, reading data from the GPS and parsing
 * full strings.
 */

/**
 * QGPSDevice::parseGGA()
 *
 * Parses a GPGGA string that contains fix information, such as
 * latitude, longitude, etc.
 *
 * The format of the GPGGA String is as follows:
 *
 *  $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
 *  |||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||
 *  01234567890123456789012345678901234567890123456789012345678901234
 *  |         |         |         |         |         |         |
 *  0         10        20        30        40        50        60
 *
 *  GPGGA       -   Global Positioning System Fix Data
 *  123519      -   Fix taken at 12:35:19 UTC
 *  4807.038,N  -   Latitude 48 deg 07.038' N
 *  01131.000,E -   Longitude 11 deg 31.000' E
 *  1           -   Fix quality:
 *                      0 = Invalid
 *                      1 = GPS fix (SPS)
 *                      2 = DGPS fix
 *                      3 = PPS fix
 *                      4 = Real time kinematic
 *                      5 = Float RTK
 *                      6 = Estimated (dead reckoning)
 *                      7 = Manual input mode
 *                      8 = Simulation mode
 *  08          -   Number of satellites being tracked
 *  0.9         -   Horizontal dissolution of precision
 *  545.4,M     -   Altitude (meters) above sea level
 *  46.9,M      -   Height of geoid (sea level) above WGS84 ellipsoid
 *  (empty)     -   Seconds since last DGPS update
 *  (empty)     -   DGPS station ID number
 *  *47         -   Checksum, begins with *
 *
 * @param char ggaString    The full NMEA GPGGA string, starting with
 *                          the $ and ending with the checksum
 */

void QGPSDevice::parseNMEA(const QByteArray& bufferString)
{
    if (bufferString.length() < 6) return;
    if(bufferString[3] == 'G' && bufferString[4] == 'G' && bufferString[5] == 'A')
    {
        //strcpy(nmeastr_gga, bufferString);
        parseGGA(bufferString.data());
    }
    else if(bufferString[3] == 'G' && bufferString[4] == 'L' && bufferString[5] == 'L')
    {
        //strcpy(nmeastr_gga, bufferString);
        parseGLL(bufferString.data());
    }
    else if(bufferString[3] == 'G' && bufferString[4] == 'S' && bufferString[5] == 'V')
    {
        //strcpy(nmeastr_gsv, bufferString);
        parseGSV(bufferString.data());
    }
    else if(bufferString[3] == 'G' && bufferString[4] == 'S' && bufferString[5] == 'A')
    {
        //strcpy(nmeastr_gsa, bufferString);
        parseGSA(bufferString.data());
    }
    else if(bufferString[3] == 'R' && bufferString[4] == 'M' && bufferString[5] == 'C')
    {
        //strcpy(nmeastr_rmc, bufferString);
        if (parseRMC(bufferString.data()))
            if (fixStatus() == QGPSDevice::StatusActive && (fixType() == QGPSDevice::Fix3D || fixType() == QGPSDevice::FixUnavailable))
                emit updatePosition(latitude(), longitude(), dateTime(), altitude(), speed(), heading());
    }
    emit updateStatus();
}

bool QGPSDevice::parseGGA(const char *ggaString)
{
    mutex->lock();

    QString line(ggaString);
    if (line.count('$') > 1)
        return false;

    QStringList tokens = line.split(",");

    qreal lat = tokens[2].left(2).toDouble();
    qreal latmin = tokens[2].mid(2).toDouble();
    lat += latmin / 60.0;
    if (tokens[3] != "N")
        lat = -lat;
    //cur_latitude = lat;

    if (!tokens[3].isEmpty())
    {
        if (tokens[3].at(0) == 'N')
            setLatCardinal(CardinalNorth);
        else if (tokens[3].at(0) == 'S')
            setLatCardinal(CardinalSouth);
        else
            setLatCardinal(CardinalNone);
    }


    qreal lon = tokens[4].left(3).toDouble();
    qreal lonmin = tokens[4].mid(3).toDouble();
    lon += lonmin / 60.0;
    if (tokens[5] != "E")
        lon = -lon;
    //cur_longitude = lon;

    if (!tokens[5].isEmpty())
    {
        if (tokens[5].at(0) == 'E')
            setLatCardinal(CardinalEast);
        else if (tokens[5].at(0) == 'W')
            setLatCardinal(CardinalWest);
        else
            setLatCardinal(CardinalNone);
    }

    int fix = tokens[6].toInt();
    setFixQuality(fix);

    int numSat = tokens[7].toInt();
    setNumSatellites(numSat);

    qreal dilut = tokens[8].toFloat();
    setDillution(dilut);

    qreal altitude = tokens[9].toFloat();
    setAltitude(altitude);

    mutex->unlock();

    return true;
} // parseGGA()

bool QGPSDevice::parseGLL(const char *ggaString)
{
    mutex->lock();

    QString line(ggaString);
    if (line.count('$') > 1)
        return false;

    QStringList tokens = line.split(",");

    qreal lat = tokens[1].left(2).toDouble();
    qreal latmin = tokens[1].mid(2).toDouble();
    lat += latmin / 60.0;
    if (tokens[2] != "N")
        lat = -lat;
    //cur_latitude = lat;

    if (!tokens[2].isEmpty())
    {
        if (tokens[2].at(0) == 'N')
            setLatCardinal(CardinalNorth);
        else if (tokens[2].at(0) == 'S')
            setLatCardinal(CardinalSouth);
        else
            setLatCardinal(CardinalNone);
    }

    qreal lon = tokens[3].left(3).toDouble();
    qreal lonmin = tokens[3].mid(3).toDouble();
    lon += lonmin / 60.0;
    if (tokens[4] != "E")
        lon = -lon;
    //cur_longitude = lon;

    if (!tokens[4].isEmpty())
    {
        if (tokens[4].at(0) == 'E')
            setLatCardinal(CardinalEast);
        else if (tokens[4].at(0) == 'W')
            setLatCardinal(CardinalWest);
        else
            setLatCardinal(CardinalNone);
    }


    if (tokens[6] == "A")
    {
        setFixStatus(StatusActive);
    }
    else
    {
        setFixStatus(StatusVoid);
    }

    mutex->unlock();

    return true;
} // parseGGA()

/**
 * QGPSDevice::parseGSA()
 *
 * Parses a GPGSA string that contains information about the nature
 * of the fix, such as DOP (dillution of precision) and active satellites
 * based on the viewing mask and almanac data of the reciever.
 *
 * The format of the GPGSA String is as follows:
 *
 *  $GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39
 *  |||||||||||||||||||||||||||||||||||||||||||||||
 *  01234567890123456789012345678901234567890123456
 *  |         |         |         |         |
 *  0         10        20        30        40
 *
 *  GPGSA       -   Information about satellite status
 *  A           -   Fix mode, (A)utomatic or (M)anual
 *  3           -   Fix type:
 *                      1 = Invalid
 *                      2 = 2D
 *                      3 = 3D (4 or more satellites)
 *  04,05,...   -   Satellites used in the solution (up to 12)
 *  2.5         -   DOP (dillution of precision)
 *  1.3         -   Horizontal DOP
 *  2.1         -   Vertical DOP
 *  *39         -   Checksum
 *
 * @param char  The full NMEA GPGSA string, from $ to checksum
 */

bool QGPSDevice::parseGSA(const char *gsaString)
{
    mutex->lock();

    QString line(gsaString);
    if (line.count('$') > 1)
        return false;

    QStringList tokens = line.split(",");

    QString autoSelectFix = tokens[1];
    if(autoSelectFix == "A")
        setFixMode(FixAuto);
    else
        setFixMode(FixManual);

    int fix = tokens[2].toInt();
    if(fix == 1)
        setFixType(FixInvalid);
    else if(fix == 2)
        setFixType(Fix2D);
    else
        setFixType(Fix3D);

    for(int index = 0; index < 12; index ++) {
        activeSats[index] = tokens[index+3].toInt();
    }

    mutex->unlock();

    return true;
} // parseGSA()

/**
 * QGPSDevice::parseRMC()
 *
 * Parses an RMC string, which contains the recommended minimum fix
 * data, such as latitude, longitude, altitude, speed, track angle,
 * date, and magnetic variation. Saves us some calculating :)
 *
 * The format of the GPRMC string is as follows:
 *
 *  $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
 *  ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||
 *  01234567890123456789012345678901234567890123456789012345678901234567
 *  |         |         |         |         |         |         |
 *  0         10        20        30        40        50        60
 *
 *  GPRMC       -   Recommended minimum fix data
 *  123519      -   Fix taken at 12:35:19 UTC
 *  A           -   Fix status, (A)ctive or (V)oid
 *  4807.038,N  -   Latitude 48 degrees 07.038' N
 *  01131.000,E -   Longitude 11 degrees, 31.000' E
 *  022.4       -   Ground speed in knots
 *  084.4       -   Track angle in degrees (true north)
 *  230394      -   Date: 23 March 1994
 *  003.1,W     -   Magnetic Variation
 *  *6A         -   Checksum
 *
 * @param char  Full RMC string, from $ to checksum
 */

bool QGPSDevice::parseRMC(const char *rmcString)
{
    mutex->lock();

    // Fix time

    QString line(rmcString);
    if (line.count('$') > 1)
        return false;

    QStringList tokens = line.split(",");

    QString strDate = tokens[9] + tokens[1];
    cur_datetime = QDateTime::fromString(strDate, "ddMMyyHHmmss.zzz");

    if (cur_datetime.date().year() < 1970)
        cur_datetime = cur_datetime.addYears(100);

    // Fix status

    if (tokens[2] == "A")
    {
        setFixStatus(StatusActive);
    }
    else
    {
        setFixStatus(StatusVoid);
    }

    // Latitude

    qreal lat = tokens[3].left(2).toDouble();
    qreal latmin = tokens[3].mid(2).toDouble();
    lat += latmin / 60.0;
    if (tokens[4] != "N")
        lat = -lat;
    cur_latitude = lat;

    if (!tokens[4].isEmpty())
    {
        if (tokens[4].at(0) == 'N')
            setLatCardinal(CardinalNorth);
        else if (tokens[4].at(0) == 'S')
            setLatCardinal(CardinalSouth);
        else
            setLatCardinal(CardinalNone);
    }

    qreal lon = tokens[5].left(3).toDouble();
    qreal lonmin = tokens[5].mid(3).toDouble();
    lon += lonmin / 60.0;
    if (tokens[6] != "E")
        lon = -lon;
    cur_longitude = lon;

    if (!tokens[6].isEmpty())
    {
        if (tokens[6].at(0) == 'E')
            setLatCardinal(CardinalEast);
        else if (tokens[6].at(0) == 'W')
            setLatCardinal(CardinalWest);
        else
            setLatCardinal(CardinalNone);
    }

    // Ground speed in km/h

    qreal speed = QString::number(tokens[7].toDouble() * 1.852, 'f', 1).toDouble();
    setSpeed(speed);

    // Heading

    qreal heading = tokens[8].toDouble();
    setHeading(heading);

    // Magnetic variation

    qreal magvar = tokens[10].toDouble();
    setVariation(magvar);

    if (!tokens[11].isEmpty())
    {
        if (tokens[11].at(0) == 'E')
            setVarCardinal(CardinalEast);
        else if (tokens[11].at(0) == 'W')
            setVarCardinal(CardinalWest);
        else
            setVarCardinal(CardinalNone);
    }

    mutex->unlock();

    return true;
} // parseRMC()

/**
 * QGPSDevice::parseGSV()
 *
 * Parses a GPGSV string, which contains satellite position and signal
 * strenght information. parseGSV() fills the satArray array with the
 * PRNs, elevations, azimuths, and SNRs of the visible satellites. This
 * array is based on the position of the satellite in the strings, not
 * the order of the PRNs! (see README for info)
 *
 * The format of the GPGSV string is as follows:
 *
 *  $GPGSV,2,1,08,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45*75
 *  ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||
 *  01234567890123456789012345678901234567890123456789012345678901234567
 *  |         |         |         |         |         |         |
 *  0         10        20        30        40        50        60
 *
 *  GPGSV       -   Satellite status
 *  2           -   Number of GPGSV sentences for full data
 *  1           -   Current sentence (1 of 2, etc)
 *  08          -   Number of satellites in view
 *
 *  01          -   Satellite PRN
 *  40          -   Elevation, degrees
 *  083         -   Azimuth, degrees
 *  46          -   SNR (signal to noise ratio)
 *      (for up to four satellites per sentence)
 *  *75         -   Checksum
 */

bool QGPSDevice::parseGSV(const char *gsvString)
{
    mutex->lock();

    int totalSentences;
    int currentSentence;
    int totalSatellites;
    int prn, elev, azim, snr;

    QString line(gsvString);
    if (line.count('$') > 1)
        return false;

    QStringList tokens = line.split(",");

    totalSentences = tokens[1].toInt();
    currentSentence = tokens[2].toInt();
    totalSatellites = tokens[3].toInt();

    qDebug() << "Parsing GSV string " << gsvString;
    qDebug() << " --> sentence " << currentSentence << " of " << totalSentences << ", " << totalSatellites << " total satellites in view";

    for(int i = 0; (i < 4) && ((i*4)+4+3 < tokens.size()); i ++) {
        prn = tokens[(i*4)+4].toInt();
        elev = tokens[(i*4)+4+1].toInt();
        azim = tokens[(i*4)+4+2].toInt();
        if (tokens[(i*4)+4+3].contains('*')) {
            QStringList tok2 = tokens[(i*4)+4+3].split("*");
            snr = tok2[0].toInt();
        } else
            snr = tokens[(i*4)+4+3].toInt();
        satArray[prn][0] = elev;
        satArray[prn][1] = azim;
        satArray[prn][2] = snr;
    }

    mutex->unlock();

    return true;
}

/**
 * QGPSDevice::startDevice()
 *
 * Calls start() to begin thread execution
 */

void QGPSDevice::startDevice()
{
    mutex->lock();
    stopLoop = false;
    mutex->unlock();

    //printf("We're starting...\n");

    start();
}

/**
 * QGPSDevice::stopDevice()
 *
 * Stops execution of run() and ends thread execution
 * This function will be called outside this thread
 */

void QGPSDevice::stopDevice()
{
    // this is through a queued connection
    emit doStopDevice();
}

#ifndef _MOBILE
/*** QGPSComDevice  ***/

QGPSComDevice::QGPSComDevice(const QString &device)
    : QGPSDevice()
{
#ifdef Q_OS_WIN
    if (!device.isNull() && !device.startsWith("\\\\.\\"))
        setDevice("\\\\.\\" + device);
    else
#endif
    if(!device.isNull())
    {
        setDevice(device);
    }
}

QGPSComDevice::~QGPSComDevice()
{
    if (LogFile) {
        if (LogFile->isOpen())
            LogFile->close();
        delete LogFile;
    }
}

/**
 * QGPSComDevice::openDevice()
 *
 * Opens the serial port and sets the parameters for data transfer: parity,
 * stop bits, blocking, etc.
 */

bool QGPSComDevice::openDevice()
{
    port = new QextSerialPort(device());
    port->setBaudRate(BAUD4800);
    port->setFlowControl(FLOW_OFF);
    port->setParity(PAR_NONE);
    port->setDataBits(DATA_8);
    port->setStopBits(STOP_2);

    if (port->open(QIODevice::ReadOnly)) {
        if (M_PREFS->getGpsSaveLog()) {
            QString fn = "log-" + QDateTime::currentDateTime().toString(Qt::ISODate) + ".nmea";
            fn.replace(':', '-');
            LogFile = new QFile(M_PREFS->getGpsLogDir() + "/"+fn);
            if (!LogFile->open(QIODevice::WriteOnly)) {
                QMessageBox::critical(NULL, tr("GPS log error"),
                    tr("Unable to create GPS log file: %1.").arg(M_PREFS->getGpsLogDir() + "/"+fn), QMessageBox::Ok);
                delete LogFile;
                LogFile = NULL;
            }
        }
        return true;
    }
    return false;
}

/**
 * QGPSComDevice::closeDevice()
 *
 * Closes the serial port
 */

bool QGPSComDevice::closeDevice()
{
    port->close();
    if (LogFile && LogFile->isOpen()) {
        LogFile->close();
        delete LogFile;
    }
    LogFile = NULL;

    return true;
}

void QGPSComDevice::onLinkReady()
{
}

void QGPSComDevice::onStop()
{
    quit();
}


void QGPSComDevice::run()
{
    GPSSlotForwarder Forward(this);

    QTimer *timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), &Forward, SLOT(checkDataAvailable()));
    timer->start(150);

//	connect(port,SIGNAL(readyRead()),&Forward,SLOT(onDataAvailable()));
    connect(this,SIGNAL(doStopDevice()),&Forward,SLOT(onStop()));
    exec();
    closeDevice();
}

void QGPSComDevice::checkDataAvailable() {
    if (port->bytesAvailable() > 0)
        onDataAvailable();
}

void QGPSComDevice::onDataAvailable()
{
    QByteArray ba(port->readAll());
    // filter out unwanted characters
    for (int i=ba.count(); i; --i)
        if(ba[i-1] == '\0' ||
            (!isalnum((quint8)ba[i-1]) &&
             !isspace((quint8)ba[i-1]) &&
             !ispunct((quint8)ba[i-1])))
        {
            ba.remove(i-1,1);
        }
    if (LogFile)
        LogFile->write(ba);
    Buffer.append(ba);
    if (Buffer.length() > 4096)
        // safety valve
        Buffer.remove(0,Buffer.length()-4096);
    while (Buffer.count())
    {
        // look for begin of sentence marker
        int i = Buffer.indexOf('$');
        if (i<0)
        {
            Buffer.clear();
            return;
        }
        Buffer.remove(0,i);
        // look for end of sentence marker
        for (i=0; i<Buffer.count(); ++i)
            if ( (Buffer[i] == (char)(0x0a)) || (Buffer[i] == (char)(0x0d)) )
                break;
        if (i == Buffer.count())
            return;
        parseNMEA(Buffer.mid(0,i-2));
        Buffer.remove(0,i);
    }
}
#endif

/*** QGPSFileDevice  ***/

QGPSFileDevice::QGPSFileDevice(const QString &device)
    : QGPSDevice()
{
    if(!device.isNull())
    {
        setDevice(device);
    }
}

/**
 * QGPSFileDevice::openDevice()
 *
 * Opens the serial port and sets the parameters for data transfer: parity,
 * stop bits, blocking, etc.
 */

bool QGPSFileDevice::openDevice()
{
    theFile = new QFile(device());

    if (!theFile->open(QIODevice::ReadOnly | QIODevice::Text)) {
        delete theFile;
        theFile = NULL;
        return false;
    }

    return true;
}

void QGPSFileDevice::onLinkReady()
{
}

void QGPSFileDevice::onStop()
{
    quit();
}

/**
 * QGPSFileDevice::closeDevice()
 *
 * Closes the serial port
 */

bool QGPSFileDevice::closeDevice()
{
    if (theFile)
        theFile->close();

    return true;
}

void QGPSFileDevice::run()
{
    GPSSlotForwarder Forward(this);
    QTimer* t = new QTimer;
    connect(t,SIGNAL(timeout()),&Forward,SLOT(onDataAvailable()));
    connect(this,SIGNAL(doStopDevice()),&Forward,SLOT(onStop()));
    t->start(100);
    exec();
    closeDevice();
}

void QGPSFileDevice::onDataAvailable()
{

    int  index = 0;
    char bufferChar;
    char bufferString[100];

    while (theFile->read(&bufferChar, 1) && bufferChar != '$') {}
    if(bufferChar == '$')
    {
        index = 0;
        bufferString[index] = bufferChar;

        do
        {
            theFile->read(&bufferChar, 1);
            if(bufferChar != '\0' && (isalnum(bufferChar) || isspace(bufferChar) || ispunct(bufferChar)))
            {
                index ++;
                bufferString[index] = bufferChar;
            }
        } while(bufferChar != 0x0a && bufferChar != 0x0d);

        bufferString[index + 1] = '\0';

        mutex->lock();

        if(bufferString[3] == 'G' && bufferString[4] == 'G' && bufferString[5] == 'A')
        {
            //strcpy(nmeastr_gga, bufferString);
            parseGGA(bufferString);
        }
        else if(bufferString[3] == 'G' && bufferString[4] == 'L' && bufferString[5] == 'L')
        {
            //strcpy(nmeastr_gga, bufferString);
            parseGLL(bufferString);
        }
        else if(bufferString[3] == 'G' && bufferString[4] == 'S' && bufferString[5] == 'V')
        {
            //strcpy(nmeastr_gsv, bufferString);
            parseGSV(bufferString);
        }
        else if(bufferString[3] == 'G' && bufferString[4] == 'S' && bufferString[5] == 'A')
        {
            //strcpy(nmeastr_gsa, bufferString);
            parseGSA(bufferString);
        }
        else if(bufferString[3] == 'R' && bufferString[4] == 'M' && bufferString[5] == 'C')
        {
            //strcpy(nmeastr_rmc, bufferString);
            if (parseRMC(bufferString))
                if (fixStatus() == QGPSDevice::StatusActive && (fixType() == QGPSDevice::Fix3D || fixType() == QGPSDevice::FixUnavailable))
                    emit updatePosition(latitude(), longitude(), dateTime(), altitude(), speed(), heading());
        }

        mutex->unlock();

        emit updateStatus();
    }
}

#ifndef _MOBILE
/* GPSSDEVICE */

#ifdef USE_GPSD_LIB
QGPSDDevice::QGPSDDevice(const QString& device)
{
    setDevice(device);
}

bool QGPSDDevice::openDevice()
{
//	if (M_PREFS->getGpsSaveLog()) {
//		QString fn = "log-" + QDateTime::currentDateTime().toString(Qt::ISODate) + ".nmea";
//		fn.replace(':', '-');
//		LogFile = new QFile(M_PREFS->getGpsLogDir() + "/"+fn);
//		if (!LogFile->open(QIODevice::WriteOnly)) {
//			QMessageBox::critical(NULL, tr("GPS log error"),
//				tr("Unable to create GPS log file: %1.").arg(M_PREFS->getGpsLogDir() + "/"+fn), QMessageBox::Ok);
//			delete LogFile;
//			LogFile = NULL;
//		}
//	}
    return true;
}

bool QGPSDDevice::closeDevice()
{
    delete Server;
    return true;
}

// this function will be called within this thread
void QGPSDDevice::onStop()
{
    quit();
}

void QGPSDDevice::run()
{
    GPSSlotForwarder Forward(this);
//    QTcpSocket Link;
//    Server = &Link;
//    Link.connectToHost(M_PREFS->getGpsdHost(),M_PREFS->getGpsdPort());
//    connect(Server,SIGNAL(connected()),&Forward,SLOT(onLinkReady()));
//    connect(Server,SIGNAL(readyRead()),&Forward,SLOT(onDataAvailable()));
    connect(this,SIGNAL(doStopDevice()),&Forward,SLOT(onStop()));
//    exec();
    QEventLoop l;
#if GPSD_API_MAJOR_VERSION < 5
    Server = new gpsmm();
    errno = 0;
    gpsdata = Server->open(M_PREFS->getGpsdHost().toLatin1().data(),QString::number(M_PREFS->getGpsdPort()).toLatin1().data());
    if (!gpsdata) {
  #ifndef Q_OS_WIN32
        QString msg( (errno<0) ? gps_errstr(errno) : strerror(errno) );
  #else
        QString msg( (errno<0) ? "" : strerror(errno) );
  #endif
        qDebug() << tr("Unable to connect to %1:%2").arg(M_PREFS->getGpsdHost()).arg(QString::number(M_PREFS->getGpsdPort()))
                 << ": " << msg;
        return;
    }
#else
    Server = new gpsmm(M_PREFS->getGpsdHost().toLatin1().data(),QString::number(M_PREFS->getGpsdPort()).toLatin1().data());
#endif

    if (! Server->is_open()) {
       qDebug() << "GPSD connection not open. Is the daemon running?";
    }

    onLinkReady();
    l.processEvents();

    forever {
#if GPSD_API_MAJOR_VERSION > 3
#if GPSD_API_MAJOR_VERSION < 5
        if (Server->waiting())
#else
        /* Wait time in microseconds, 25000 = 25ms */
        if (Server->waiting(25000))
#endif
#endif
        {
            /* Only process data if there is a chance the server is actually working. */
            if (Server->is_open() && serverOk) {
                onDataAvailable();
            }
        }
        l.processEvents();
    }

    delete Server;
}

void QGPSDDevice::onDataAvailable()
{
    #if GPSD_API_MAJOR_VERSION < 5
       gpsdata = Server->poll();
       if (!gpsdata)
           return;
    #else
       gpsdata = Server->read();
       if (!gpsdata)
           {
           QString msg( (errno==0) ? "socket to gpsd was closed" : strerror(errno) );
           qDebug() << "gpsmm::read() failed: " << msg;
           serverOk = false;
           return;
           }
    #endif

    setFixStatus(StatusVoid);
    switch (gpsdata->fix.mode) {
    case MODE_NOT_SEEN:
        setFixType(FixUnavailable);
        return;
    case MODE_NO_FIX:
        setFixType(FixInvalid);
        return;
    case MODE_2D:
        setFixType(Fix2D);
        break;
    case MODE_3D:
        setFixType(Fix3D);
        break;
    }

    setFixStatus(StatusActive);
    setLatitude(gpsdata->fix.latitude);
    setLongitude(gpsdata->fix.longitude);
    qreal Alt = gpsdata->fix.altitude;
    if (!__isnan(Alt))
        setAltitude(Alt);
    qreal Speed = gpsdata->fix.speed;
    if (!__isnan(Speed))
        setSpeed(Speed);
    qreal Heading = gpsdata->fix.track;
    if (!__isnan(Heading))
        setHeading(Heading);
    if (gpsdata->fix.time)
        cur_datetime = QDateTime::fromTime_t(gpsdata->fix.time);
    emit updatePosition(gpsdata->fix.latitude,
                        gpsdata->fix.longitude,
                        cur_datetime,
                        cur_altitude, cur_speed, cur_heading);

#if GPSD_API_MAJOR_VERSION > 3
    int num_sat = gpsdata->satellites_visible;
#else
    int num_sat = gpsdata->satellites;
#endif
    for(int i = 0; i < 50; i ++)
        satArray[i][0] = satArray[i][1] = satArray[i][2] = 0;
    for (int i=0; i<num_sat; ++i)
    {
#if GPSD_API_MAJOR_VERSION > 5
        int id = gpsdata->skyview[i].PRN;
        satArray[id][0] = gpsdata->skyview[i].elevation;
        satArray[id][1] = gpsdata->skyview[i].azimuth;
        satArray[id][2] = gpsdata->skyview[i].ss;
#else
        int id = gpsdata->PRN[i];
        satArray[id][0] = gpsdata->elevation[i];
        satArray[id][1] = gpsdata->azimuth[i];
        satArray[id][2] = gpsdata->ss[i];
#endif
    }
    setNumSatellites(num_sat);

    emit updateStatus();
}

void QGPSDDevice::onLinkReady()
{
    if (!Server) return;
#if GPSD_API_MAJOR_VERSION > 3
    gpsdata = Server->stream(WATCH_ENABLE);
#ifndef Q_OS_WIN32
    if ( gpsdata == 0 )
        qDebug() << "gpsmm::stream() failed: " << gps_errstr(errno) << '\n';
#endif
#else
    gpsdata = Server->query("w+x\n");
#endif
}
#else /*USE_GPSD_LIB*/
QGPSDDevice::QGPSDDevice(const QString& device)
{
    setDevice(device);
}

bool QGPSDDevice::openDevice()
{
    if (M_PREFS->getGpsSaveLog()) {
        QString fn = "log-" + QDateTime::currentDateTime().toString(Qt::ISODate) + ".nmea";
        fn.replace(':', '-');
        LogFile = new QFile(M_PREFS->getGpsLogDir() + "/"+fn);
        if (!LogFile->open(QIODevice::WriteOnly)) {
            QMessageBox::critical(NULL, tr("GPS log error"),
                tr("Unable to create GPS log file: %1.").arg(M_PREFS->getGpsLogDir() + "/"+fn), QMessageBox::Ok);
            delete LogFile;
            LogFile = NULL;
        }
    }
    return true;
}

bool QGPSDDevice::closeDevice()
{
    return true;
}

// this function will be called within this thread
void QGPSDDevice::onStop()
{
    quit();
}

void QGPSDDevice::run()
{
    GPSSlotForwarder Forward(this);
    QTcpSocket Link;
    Server = &Link;
    Link.connectToHost(M_PREFS->getGpsdHost(),M_PREFS->getGpsdPort());
    connect(Server,SIGNAL(connected()),&Forward,SLOT(onLinkReady()));
    connect(Server,SIGNAL(readyRead()),&Forward,SLOT(onDataAvailable()));
    connect(this,SIGNAL(doStopDevice()),&Forward,SLOT(onStop()));
    Server->write("?WATCH={\"enable\":true,\"nmea\":true}");
    exec();
}

void QGPSDDevice::onDataAvailable()
{
    QByteArray ba(Server->readAll());
    // filter out unwanted characters
    for (int i=ba.count(); i; --i)
        if(ba[i-1] == '\0' ||
            (!isalnum((quint8)ba[i-1]) &&
             !isspace((quint8)ba[i-1]) &&
             !ispunct((quint8)ba[i-1])))
        {
            ba.remove(i-1,1);
        }
    if (LogFile)
        LogFile->write(ba);
    Buffer.append(ba);
    if (Buffer.length() > 4096)
        // safety valve
        Buffer.remove(0,Buffer.length()-4096);
    while (Buffer.count())
    {
        // look for begin of sentence marker
        int i = Buffer.indexOf('$');
        if (i<0)
        {
            Buffer.clear();
            return;
        }
        Buffer.remove(0,i);
        // look for end of sentence marker
        for (i=0; i<Buffer.count(); ++i)
            if ( (Buffer[i] == (char)(0x0a)) || (Buffer[i] == (char)(0x0d)) )
                break;
        if (i == Buffer.count())
            return;
        parseNMEA(Buffer.mid(0,i-2));
        Buffer.remove(0,i);
    }
}

void QGPSDDevice::parse(const QString& s)
{
    qDebug() << "parsing " << s.toUtf8().data() << "*";
    QStringList Args(s.split(',',QString::SkipEmptyParts));
    for (int i=0; i<Args.count(); ++i)
    {
        QString Left(Args[i].left(2));
        if (Left == "O=")
            parseO(Args[i].right(Args[i].length()-2));
        if (Left == "Y=")
            parseY(Args[i].right(Args[i].length()-2));
    }
}

void QGPSDDevice::parseY(const QString& s)
{
    for(int i = 0; i < 50; i ++)
        satArray[i][0] = satArray[i][1] = satArray[i][2] = 0;
    QStringList Sats(s.split(':',QString::SkipEmptyParts));
    for (int i=1; i<Sats.size(); ++i)
    {
        QStringList Items(Sats[i].split(' ',QString::SkipEmptyParts));
        if (Items.count() < 5)
            continue;
        int id = Items[0].toInt();
        if ( (id >= 0) && (id<50) )
        {
            satArray[id][0] = int(Items[1].toDouble());
            satArray[id][1] = int(Items[2].toDouble());
            satArray[id][2] = int(Items[3].toDouble());
        }
    }
    setNumSatellites(Sats.size());
    emit updateStatus();
}

void QGPSDDevice::parseO(const QString& s)
{
    if (s.isEmpty()) return;
    setFixType(FixInvalid);
    if (s[0] == '?') return;
    QStringList Args(s.split(' ',QString::SkipEmptyParts));
    if (Args.count() < 5) return;
    setFixType(Fix3D);
    setFixStatus(StatusActive);
    setLatitude(Args[3].toDouble());
    setLongitude(Args[4].toDouble());
    qreal Alt = 0;
    if (Args.count() > 5)
        Alt = Args[5].toDouble();
    qreal Speed = 0;
    if (Args.count() > 9)
        Speed = Args[9].toDouble();
    qreal Heading = 0;
    if (Args.count() > 7)
        Heading = Args[7].toDouble();
    emit updatePosition(Args[3].toDouble(),
        Args[4].toDouble(),
        QDateTime::currentDateTime(),
        Alt, Speed, Heading);
    setHeading(Heading);
    setAltitude(Alt);
    setSpeed(Speed);
    emit updateStatus();

}

void QGPSDDevice::onLinkReady()
{
    if (!Server) return;
    Server->write("w+");
    Server->write("r+");
    Server->write("j=1");
}
#endif /*USE_GPSD_LIB*/

#endif

#if defined Q_OS_SYMBIAN || defined(Q_WS_SIMULATOR)

/* QtMobility */

// Use the QtMobility namespace
QTM_USE_NAMESPACE

QGPSMobileDevice::QGPSMobileDevice()
{
}

bool QGPSMobileDevice::openDevice()
{
    src = QGeoPositionInfoSource::createDefaultSource(this);
    if (!src) {
        return false;
    }
    src->setUpdateInterval(1000);
    src->startUpdates();

    connect(src, SIGNAL(updateTimeout()), SLOT(onUpdateTimeout()));
    connect(src, SIGNAL(positionUpdated(const QGeoPositionInfo&)), SLOT(onPositionUpdated(const QGeoPositionInfo&)));

    satsrc = QGeoSatelliteInfoSource::createDefaultSource(this);
    if (satsrc) {
        connect(satsrc, SIGNAL(satellitesInViewUpdated(QList<QGeoSatelliteInfo>)), SLOT(on_satellitesInViewUpdated(QList<QGeoSatelliteInfo>)));
        connect(satsrc, SIGNAL(satellitesInUseUpdated(QList<QGeoSatelliteInfo>)), SLOT(on_satellitesInUseUpdated(QList<QGeoSatelliteInfo>)));
        connect(satsrc, SIGNAL(requestTimeout()), SLOT(on_satRequestTimeout()));

        satsrc->startUpdates();
    }
    return true;
}

bool QGPSMobileDevice::closeDevice()
{
    return true;
}

// this function will be called within this thread
void QGPSMobileDevice::onStop()
{
    quit();
}

void QGPSMobileDevice::run()
{
    GPSSlotForwarder Forward(this);
    connect(this,SIGNAL(doStopDevice()),&Forward,SLOT(onStop()));

    exec();

    src->stopUpdates();
}

void QGPSMobileDevice::onUpdateTimeout()
{
    setFixType(FixUnavailable);
}

void QGPSMobileDevice::onPositionUpdated(const QGeoPositionInfo &update)
{
    cur_datetime = update.timestamp();
    cur_latitude = update.coordinate().latitude();
    cur_longitude = update.coordinate().longitude();

    cur_altitude = update.coordinate().altitude();

    if (update.hasAttribute(QGeoPositionInfo::GroundSpeed)) {
        cur_speed = update.attribute(QGeoPositionInfo::GroundSpeed);
    }
    if (update.hasAttribute(QGeoPositionInfo::HorizontalAccuracy)) {
        m_accuracy = qRound(update.attribute(QGeoPositionInfo::HorizontalAccuracy));
    }
    if (update.hasAttribute(QGeoPositionInfo::Direction)) {
        cur_heading = qRound(update.attribute(QGeoPositionInfo::Direction));
    }

    if (m_accuracy > 500) {
        setFixStatus(StatusVoid);
        setFixType(FixUnavailable);
    } else if (m_accuracy < 100) {
        setFixStatus(StatusActive);
        setFixType(Fix3D);
    } else {
        setFixStatus(StatusActive);
        setFixType(Fix2D);
    }

    emit updatePosition(latitude(), longitude(), dateTime(), altitude(), speed(), heading());
    emit updateStatus();
}

//int QGPSMobileDevice::getUpdateInterval() const
//{
//    if (src)
//        return src->updateInterval();
//    else
//        return 0;
//}

//void QGPSMobileDevice::setUpdateInterval(int arg)
//{
//    if (src)
//        src->setUpdateInterval(arg);
//}

void QGPSMobileDevice::on_satellitesInViewUpdated(QList<QGeoSatelliteInfo> satList)
{
    qDebug() << "Sat updated";
    m_List = satList;
    emit updateStatus();
}

void QGPSMobileDevice::on_satellitesInUseUpdated(QList<QGeoSatelliteInfo> satList)
{
    m_UseList = satList;
    emit updateStatus();
}

void QGPSMobileDevice::on_satRequestTimeout()
{
    m_List.clear();
    m_UseList.clear();
    emit updateStatus();
}

void QGPSMobileDevice::satInfo(int index, int &elev, int &azim, int &snr)
{
    elev = 0;
    azim = 0;
    snr = 0;

    foreach (QGeoSatelliteInfo gi, m_List) {
        if (gi.prnNumber() == index) {
            elev = gi.attribute(QGeoSatelliteInfo::Elevation);
            azim = gi.attribute(QGeoSatelliteInfo::Azimuth);
            snr = gi.signalStrength();
            return;
        }
    }
}

void QGPSMobileDevice::onLinkReady()
{
}

void QGPSMobileDevice::onDataAvailable()
{
}


#endif
