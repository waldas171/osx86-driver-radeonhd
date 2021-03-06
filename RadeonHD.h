/*
 *  RadeonHD.h
 *  RadeonHD
 *
 *  Created by Dong Luo on 5/19/08.
 *  Copyright 2008. All rights reserved.
 *
 */

#ifndef _RADEONHD_H
#define _RADEONHD_H

#include <IOKit/ndrvsupport/IONDRVFramebuffer.h>
#include "xf86str.h"

class IONDRV : public OSObject
{
    OSDeclareAbstractStructors(IONDRV)
	
public:
    virtual IOReturn getSymbol( const char * symbolName,
							   IOLogicalAddress * address ) = 0;
	
    virtual const char * driverName( void ) = 0;
	
    virtual IOReturn doDriverIO( UInt32 commandID, void * contents,
								UInt32 commandCode, UInt32 commandKind ) = 0;
};

class NDRVHD : public IONDRV
{
    OSDeclareAbstractStructors(NDRVHD)
	
private:
    enum { kIOBootNDRVDisplayMode = 100 };
	
	UInt32							modeCount;
	IODisplayModeID					*modeIDs;
	IODetailedTimingInformationV2	*modeTimings;
	Fixed							*refreshRates;
	
    void *	fAddress;
    UInt32	fRowBytes;
    UInt32	fWidth;
    UInt32	fHeight;
    UInt32	fBitsPerPixel;
	UInt32	fBitsPerComponent;
	IOIndex	fDepth;
	
	IODisplayModeID	fMode;
	Fixed	fRefreshRate;
	
	GammaTbl		*gTable;
	
	int				nubIndex;
	bool			RHDReady;
	UserOptions		*options;
	
	UInt32			fLastPowerState;
	
public:
	
    static IONDRV * fromRegistryEntry( IOService * nub );
	
    virtual void free( void );
	
    virtual IOReturn getSymbol( const char * symbolName,
							   IOLogicalAddress * address );
	
    virtual const char * driverName( void );
	
    virtual IOReturn doDriverIO( UInt32 commandID, void * contents,
								UInt32 commandCode, UInt32 commandKind );
/*
	bool hasDDCConnect( void );
	UInt8 *getDDCBlock( void );
	bool isInternalDisplay(void);
*/
private:
	
    static bool getUInt32Property( IORegistryEntry * regEntry, const char * name,
								  UInt32 * result );
    IOReturn doControl( UInt32 code, void * params );
    IOReturn doStatus( UInt32 code, void * params );
	void setModel(IORegistryEntry *device);
};


class RadeonHD : public IONDRVFramebuffer
{
    OSDeclareDefaultStructors(RadeonHD)
	
protected:

private:
	

public:
	/*! @function setCursorImage
	 @abstract Set a new image for the hardware cursor.
	 @discussion IOFramebuffer subclasses may implement hardware cursor functionality, if so they should implement this method to change the hardware cursor image. The image should be passed to the convertCursorImage() method with each type of cursor format the hardware supports until success, if all fail the hardware cursor should be hidden and kIOReturnUnsupported returned.
	 @param cursorImage Opaque cursor description. This should be passed to the convertCursorImage() method to convert to a format specific to the hardware.
	 @result An IOReturn code.
	 */
	
    //virtual IOReturn setCursorImage( void * cursorImage );
	
	/*! @function setCursorState
	 @abstract Set a new position and visibility for the hardware cursor.
	 @discussion IOFramebuffer subclasses may implement hardware cursor functionality, if so they should implement this method to change the position and visibility of the cursor.
	 @param x Left coordinate of the cursor image. A signed value, will be negative if the cursor's hot spot and position place it partly offscreen.
	 @param y Top coordinate of the cursor image. A signed value, will be negative if the cursor's hot spot and position place it partly offscreen.
	 @param visible Visible state of the cursor.
	 @result An IOReturn code.
	 */
	
    //virtual IOReturn setCursorState( SInt32 x, SInt32 y, bool visible );
	
    // Controller attributes
	
    virtual IOService * probe(  IOService *     provider,
							  SInt32 *        score );
	
    virtual IOReturn setAttribute( IOSelect attribute, uintptr_t value );
    virtual IOReturn getAttribute( IOSelect attribute, uintptr_t * value );
	
    virtual IOReturn doDriverIO( UInt32 commandID, void * contents,
								UInt32 commandCode, UInt32 commandKind );
	
	/*! @function setAttributeForConnection
	 @abstract Generic method to set some attribute of the framebuffer device, specific to one display connection.
	 @discussion IOFramebuffer subclasses may implement this method to allow arbitrary attribute/value pairs to be set, specific to one display connection. 
	 @param attribute Defines the attribute to be set. Some defined attributes are:<br> 
	 kIOCapturedAttribute If the device supports hotplugging displays, it should disable the generation of hot plug interrupts when the attribute kIOCapturedAttribute is set to true.
	 @param value The new value for the attribute.
	 @result an IOReturn code.
	 */
	
    virtual IOReturn setAttributeForConnection( IOIndex connectIndex,
											   IOSelect attribute, uintptr_t  info );

	/*! @function getAttributeForConnection
	 @abstract Generic method to retrieve some attribute of the framebuffer device, specific to one display connection.
	 @discussion IOFramebuffer subclasses may implement this method to allow arbitrary attribute/value pairs to be returned, specific to one display connection. 
	 @param attribute Defines the attribute to be returned. Some defined attributes are:<br> 
	 kConnectionSupportsHLDDCSense If the framebuffer supports the DDC methods hasDDCConnect() and getDDCBlock() it should return success (and no value) for this attribute.<br>
	 kConnectionSupportsLLDDCSense If the framebuffer wishes to make use of IOFramebuffer::doI2CRequest software implementation of I2C it should implement the I2C methods setDDCClock(), setDDCData(), readDDCClock(), readDDCData(), and it should return success (and no value) for this attribute.<br>
	 @param value Returns the value for the attribute.
	 @result an IOReturn code.
	 */
	
    virtual IOReturn getAttributeForConnection( IOIndex connectIndex,
											   IOSelect attribute, uintptr_t  * value );
	
    virtual IOReturn setDisplayMode( IODisplayModeID displayMode,
									IOIndex depth );
/*	
    virtual bool hasDDCConnect( IOIndex connectIndex );
	
	virtual IOReturn getDDCBlock( IOIndex, // connectIndex
								 UInt32 blockNumber,
								 IOSelect blockType,
								 IOOptionBits options,
								 UInt8 * data, IOByteCount * length );
*/ 
	
};

#endif /* ! _RADEONHD_H */