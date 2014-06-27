/**
 * \addtogroup w5100
 * @{
 */

/**
 * \file Driver implementation for interfacing with the W5100 device
 * \author Chuck Erhardt (chuck@e2forlife.com)
 *
 * This file contains the main driver implementation for control
 * of the W5100 device.
*/

/*
 * Change Log:
 * 
 * V1.1
 * - Modified the SPI access function to remove hard wait for READ_WRITE_DELAY  <CE>
 * - Removed READ_WRITE_DELAY Parameter <CE>
 * - Added validator to SS_NUM parameter to ensure that the value is the range 0-3 <CE>
 * - Removed "inline" keywords from interal register access function to promote
 *   compatibility with Keil PSoC3 compilers that don't support them. <CE>
 * - Fixed bug in _ProcessRxData() that caused the read pointer not to be updated without
 *   the lookahead flag being set. <CE>
 * V1.2
 * - Added _STS_SPI_IDLE flag to _SpiDone conditions to prevent deadlocking that
 *   was occuring during the first read or write using the SPI device (SPIM moded) <CE>
 * - Removed the check for SOCKET_ESTABLISHED at the start of the TcpReceive
 *   function to allow for reading received socket data after the socket was dropped
 *   by the remote server.  It was brought to my attention that this occurs when
 *   operating with client connection to HTTP servers. <CE>
 * - Added a MAC address parser and MAC to String Converter. <CE>
 */

/* Cypress library includes */
#include <cytypes.h>
#include <cylib.h>

/*  include functions and types for the driver */
#include "W5100.h"
/* include the serial interface functions */
#include <SPI.h>

extern uint8 SPI_initVar;

typedef struct
{
	uint8  Protocol;
	uint8  SocketFlags;
	uint16 SourcePort;
	uint8  ServerFlag;
} W5100_SOCKET;

#define W5100_SOCKET_TX_BASE(s)    ( 0x4000 + (s<<15) )
#define W5100_SOCKET_RX_BASE(s)    ( 0x6000 + (s<<15) )

static W5100_SOCKET W5100_SocketConfig[4];
static uint32 W5100_SubnetMask;

static uint8 W5100_MAC[6]; /* V1.2: removed = {00-DE-AD-BE-EF-00}; */

/* ------------------------------------------------------------------------ */
/* V1.2 HEX digit conversion tools for MAC Address parsing */
#define W5100_ISXDIGIT(x) \
( ((x>='0')&&(x<='9'))||((x>='a')&&(x<='f'))||((x>='A')&&(x<='F')) )
/* ------------------------------------------------------------------------ */
#define W5100_HEX2BIN(x) \
( ((x>='0')&&(x<='9'))? (x-'0') : \
  ((x>='a')&&(x<='f'))? ((x-'a')+10) : \
  ((x>='A')&&(x<='F'))? ((x-'A')+10) : 0 )
/* ------------------------------------------------------------------------ */
#define W5100_BIN2HEX(x) \
( (x>9)? ((x-10)+'A') : (x + '0'))
/* END V1.2 defines */
/* ------------------------------------------------------------------------ */

/* ======================================================================== */
/* Generic SPI Functions */
/*
 * NOTE: when using the SPIM, there must be a buffer length of at least 4
 * bytes to allow the transfer/receive cycle for each read/write operation
 * to be contained within the FIFO buffer.
 */
#if !defined(CY_SCB_SPI_H)
/* ------------------------------------------------------------------------ */	
/* V1.1 : Macro definition for the SpiDone flag. */
/*  
	V1.2 : Added SPI_IDLE flag to the condition for done to eliminate
	deadlocking when IDLE but not done.  This seems to occur during the
	initial write/read to/from the SPI port.
*/
/**
 * \brief Macro to determine the state of the spi done
 * This macro reads the status register of the transmitter and masks off the doen bit.
 */
#define W5100_SpiDone     (SPI_ReadTxStatus() & (SPI_STS_SPI_DONE | SPI_STS_SPI_IDLE))
/* ------------------------------------------------------------------------ */
/**
 * \brief Select the active SCB chip select connected to the W51
 *
 * this fucntion will set the active SS line within the SCB to select and
 * communicate with the W5100 device.
 * \note this functions requires that the device chip select is originating
 *  from the SPI device.
 */
static void W5100_W51_Select( void )
{
	/*
	 * User code for setting the chip select enable.  This is called once
	 * at the beginning of each tranfer to/from the W5100, so we recommend
	 * either doing nothing (SPIM SS straight out), or to switch the demux
	 * component to select the correct output for the SS routing.  This
	 * should probably not drive a pin directly since there is no return
	 * call from the driver that would allow an enable/disable type of
	 * implementation for CSN control.
	 */
	/* `#START CHIP_SELECT_GENERATION` */

	/* `#END` */
}
/* ------------------------------------------------------------------------ */
/**
 * \brief write Data to the W5100 at the specified address
 * \param addr Address of register or memory buffer
 * \param dat Data to write to the device
 *
 * This function will send the WRITE opcode, address and data to the W5100
 * in order to write the data to the specified register.
 */
void W5100_W51_Write(uint16 addr, uint8 dat)
{
	/* V1.1: Wait for SPI operation to complete */
	while( W5100_SpiDone == 0) {
		CyDelayUs(1);
	}
	/* V1.1: End change */
	
	/* Using internal device SS generation */
	W5100_W51_Select();
	/*
	 * The first thing to do is to clear the Rx Buffer of any lingering
	 * data left from other transfers.  Then send the data per the W5100
	 * datasheet p.62
	 */
	SPI_ClearRxBuffer();
	/* Begin transfer of data to device */
	SPI_WriteTxData(W5100_WRITE_OP);
	SPI_WriteTxData((addr>>8)&0x00FF);
	SPI_WriteTxData(addr&0x00FF);
	SPI_WriteTxData(dat);

}

/* ------------------------------------------------------------------------ */
/**
 * \brief Read data from the W5100 at the spsecified address
 * \param addr Address from which to read
 * \returns the data read from the address
 *
 * this fucntion will access a W5100 memory location and read the contents
 * using the serial protocol specified on P.61 of the datasheet.
 */
uint8 W5100_W51_Read(uint16 addr)
{
	uint32 dat;
	uint32 count;
	
	/* V1.1: Wait for SPI operation to complete */
	while( W5100_SpiDone == 0) {
		CyDelayUs(1);
	}
	/* V1.1: End change */

	/* Using internal device SS generation */
	W5100_W51_Select();
	/*
	 * First, clear the Rx Buffer of any waiting data.  Then begin the
	 * read operation by sending the op code, followed byte the address
	 * from which the read will be executed, then send a dummy byte
	 * of zeros to read the data from the device.
	 */
	SPI_ClearRxBuffer();
	/* Begin data read */
	SPI_WriteTxData(W5100_READ_OP);
	SPI_WriteTxData(addr>>8);
	SPI_WriteTxData(addr&0x00FF);
	SPI_WriteTxData( 0 );
	
	/* Wait for operations to complete. */
	/* V1.1: Wait for SPI operation to complete */
	while( W5100_SpiDone == 0) {
		CyDelayUs(1);
	}
	/* V1.1: End change */

	count = SPI_GetRxBufferSize();
	while( count > 0 )
	{
		dat = SPI_ReadRxData(); 
		count = SPI_GetRxBufferSize();
	}
	
	return( dat&0xFF );
}
/* ======================================================================== */
/* SCB Specific Functions */
#else
/* include SPI function header for the SCB */
#include <SPI_SPI_UART.h>

/* V1.1 : Include the header for the select pin used. */
#if (1 == 0)
#include <SPI_ss0_m.h>
#define W5100_SpiDone    (SPI_ss0_m_Read())
#elif (1 == 1)
#include <SPI_ss1_m.h>
#define W5100_SpiDone    (SPI_ss1_m_Read())
#elif (1 == 2)
#include <SPI_ss2_m.h>
#define W5100_SpiDone    (SPI_ss2_m_Read())
#elif (1 == 3)
#include <SPI_ss3_m.h>
#define W5100_SpiDone    (SPI_ss3_m_Read())
#else
#include <SPI_ss0_m.h>
#define W5100_SpiDone    (SPI_ss0_m_Read())
#endif
/* ------------------------------------------------------------------------ */
/**
 * \brief Select the active SCB chip select connected to the W51
 *
 * this fucntion will set the active SS line within the SCB to select and
 * communicate with the W5100 device.
 */
static void W5100_W51_Select( void )
{
#if (1 == 0)
	SPI_SpiSetActiveSlaveSelect( SPI_SPIM_ACTIVE_SS0 );
#elif (1 == 1)
	SPI_SpiSetActiveSlaveSelect( SPI_SPIM_ACTIVE_SS1 );
#elif (1 == 2)
	SPI_SpiSetActiveSlaveSelect( SPI_SPIM_ACTIVE_SS2 );
#elif (1 == 3)
	SPI_SpiSetActiveSlaveSelect( SPI_SPIM_ACTIVE_SS3 );
#else
		SPI_SpiSetActiveSlaveSelect(0);
#endif
}
/* ------------------------------------------------------------------------ */
/**
 * \brief write Data to the W5100 at the specified address
 * \param addr Address of register or memory buffer
 * \param dat Data to write to the device
 *
 * This function will send the WRITE opcode, address and data to the W5100
 * in order to write the data to the specified register.
 */
void W5100_W51_Write(uint16 addr, uint8 dat)
{
	/* V1.1: Wait for SPI operation to complete */
	while( W5100_SpiDone == 0) {
		CyDelayUs(1);
	}
	/* V1.1: End change */

	/* Using internal device SS generation */
	W5100_W51_Select();
	/*
	 * The first thing to do is to clear the Rx Buffer of any lingering
	 * data left from other transfers.  Then send the data per the W5100
	 * datasheet p.62
	 */
	SPI_SpiUartClearRxBuffer();
	/* Begin transfer of data to device */
	SPI_SpiUartWriteTxData(W5100_WRITE_OP);
	SPI_SpiUartWriteTxData((addr>>8)&0x00FF);
	SPI_SpiUartWriteTxData(addr&0x00FF);
	SPI_SpiUartWriteTxData(dat);
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Read data from the W5100 at the spsecified address
 * \param addr Address from which to read
 * \returns the data read from the address
 *
 * this fucntion will access a W5100 memory location and read the contents
 * using the serial protocol specified on P.61 of the datasheet.
 */
uint8 W5100_W51_Read(uint16 addr)
{
	uint32 dat;
	uint32 count;
	
	/* V1.1: Wait for SPI operation to complete */
	while( W5100_SpiDone == 0) {
		CyDelayUs(1);
	}
	/* V1.1: End change */

	/* Using internal device SS generation */
	W5100_W51_Select();
	/*
	 * First, clear the Rx Buffer of any waiting data.  Then begin the
	 * read operation by sending the op code, followed byte the address
	 * from which the read will be executed, then send a dummy byte
	 * of zeros to read the data from the device.
	 */
	SPI_SpiUartClearRxBuffer();
	/* Begin data read */
	SPI_SpiUartWriteTxData(W5100_READ_OP);
	SPI_SpiUartWriteTxData(addr>>8);
	SPI_SpiUartWriteTxData(addr&0x00FF);
	SPI_SpiUartWriteTxData( 0 );
	/* Wait for operations to complete. */
	/* V1.1: Wait for SPI operation to complete */
	while( W5100_SpiDone == 0) {
		CyDelayUs(1);
	}
	/* V1.1: End change */

	count = SPI_SpiUartGetRxBufferSize();
	while( count > 0 )
	{
		dat = SPI_SpiUartReadRxData(); 
		count = SPI_SpiUartGetRxBufferSize();
	}
	
	return( dat&0xFF );
}

#endif

/* ======================================================================== */
/* W5100 Register Access Primitaves */
#if (1)
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a MAC (hardware) address to a group of registers
 * \param addr The starting address of the registers/memory for the MAC address
 * \param *mac pointer to the array of bytes holding the MAC address.
 */
static void W5100_W51_SetMAC(uint16 addr, uint8 *mac)
{
	uint32 index;
	
	for(index = 0;index<6;++index)
	{
		W5100_W51_Write(addr+index,mac[index]);
	}
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a MAC (hardware) address from a set of registers.
 * \param addr the address from which the data will be read
 * \param *mac pointer to the array of bytes to hold the output mac address.
 */
static void W5100_W51_GetMAC( uint16 addr, uint8* mac )
{
	uint32 index;
	for(index=0;index<6;++index) {
		mac[index] = W5100_W51_Read(addr+index);
	}
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Write an IPv4 address to the device.
 * \param addr the address of the memory/registers to write the IPv4 Address
 * \param ip the IP address to write
 */
static void W5100_W51_SetIP( uint16 addr, uint32 ip )
{
	uint32 index;
	uint8* buffer;

	buffer = (uint8*)&ip;
	for(index = 0;index<4;++index) {
		W5100_W51_Write(addr+index,buffer[index]);
	}
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Read an IPv4 address from a set of registers
 * \param addr the address from which the IP address will be retrieved.
 * \returns the IP address read from the device.
 */
static uint32 W5100_W51_GetIP( uint16 addr )
{
	uint8 buffer[4];
	int index;
	uint32* ip;
	
	ip = (uint32*) &buffer[0];
	*ip = 0;
	for(index=0;index<4;++index) {
		buffer[index] = W5100_W51_Read(addr+index);
	}
	return( *ip );
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a 16-bit word to the device
 * \param addr the starting address to which the data will be written
 * \param val the 16-bit value to write
 */
static void W5100_W51_Write16( uint16 addr, uint16 val )
{
	W5100_W51_Write( addr, val>>8);
	W5100_W51_Write( addr + 1, val&0x00FF);
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a 16-bit value from the device
 * \param addr The starting address from which data will be read
 * \returns the 16-bit value read from the memory.
 */
static uint16 W5100_W51_Read16( uint16 addr )
{
	uint16 val;
	
	val = W5100_W51_Read( addr );
	val <<= 8;
	val += W5100_W51_Read( addr + 1);
	
	return( val );
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a block of data to consecutive memory addresses
 * \param addr the starting address of the write
 * \param *buffer pointer to the data buffer holding the data to write
 * \param length the length of data to be written
 */
static void W5100_W51_WriteBlock(uint16 addr, uint8* buffer, uint16 length)
{
	int index;
	for(index=0;index<length;++index) {
		W5100_W51_Write(addr+index,buffer[index]);
	}
}
/* ------------------------------------------------------------------------ */
/**
 * \brief read a block of data from consecutive memory addresses
 * \param addr the address from which the read will begin
 * \param *buffer pointer to the memory buffer which will hold the read data
 * \param length the length of data to read from the buffer.
 */
static void W5100_W51_ReadBlock(uint16 addr, uint8* buffer, uint16 length)
{
	int index;
	
	for(index=0;index<length;++index) {
		buffer[index] = W5100_W51_Read(addr+index);
	}
}
/* ======================================================================== */
/* END SECTION */
#endif
/* ======================================================================== */
/* Chip Register access */
#if (1)
// Common Registers
/* ------------------------------------------------------------------------ */
/**
 * \brief Set teh chip mac address to teh specified address
 * \param *mac pointer to the array holding the MAC address
 */
static void W5100_SetSourceMAC( uint8* mac ) { W5100_W51_SetMAC(9,mac); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the chip source MAC address from the MAC registers
 * \param *mac pointer to the address of the buffer to hold the read MAC address
 */
static void W5100_GetSourceMAC( uint8* mac ) { W5100_W51_GetMAC(9,mac); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Set the IPv4 address of the network gateway
 * \param ip IP address of the gateway
 */
static void W5100_SetGatewayAddress(uint32 ip) { W5100_W51_SetIP(1,ip); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the gateway address from the device
 * \returns the IP address of the gateway
 */
static uint32 W5100_GetGatewayAddress( void ) { return W5100_W51_GetIP(1); }
/* ------------------------------------------------------------------------ */
/**
 * \brief set the subnet mask of the ethernet device
 * \param the subnet mask (IPv4)
 */
static void W5100_SetSubnetMask( uint32 ip ) { W5100_W51_SetIP(5, ip); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the subnet mask from the device
 * \returns the subnet mask that was read from the device.
 */
static uint32 W5100_GetSubnetMask( void ) { return W5100_W51_GetIP(5); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write the device source IPv4 address
 * \param ip the ip address to set as the chip IP address
 */
static void W5100_SetSourceIP( uint32 ip ) { W5100_W51_SetIP( 0x0F, ip ); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the device's programmed IP address
 * \returns The IPv4 address to which the device is set.
 */
static uint32 W5100_GetSourceIP( void ) { return W5100_W51_GetIP( 0x0F ); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the device internal MODE register
 * \param mode the value to be written to the mode register.
 */
static void W5100_SetMode( uint8 mode ) { W5100_W51_Write(0,mode); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the present contents of the device internal mode register
 * \returns the value of the mode register
 */
static uint8 W5100_GetMode( void ) { return W5100_W51_Read(0); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write the value of the interrupt register
 * \param ir the value to be written to the interrupt register
 */
static void W5100_SetIR( uint8 ir ) { W5100_W51_Write(0x0015,ir); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the contents of the interrupt register
 * \returns the contents read from the interrupt register
 */
static uint8 W5100_GetIR( void ) { return W5100_W51_Read(0x0015); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Set the value of the interrupt mask register
 * \param imr the value to be written to the interrupt mask register
 */
static void W5100_SetIMR( uint8 imr) { W5100_W51_Write( 0x0016, imr ); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the interrupt mask register
 * \returns the contents of the interrupt mask register
 */
static uint8 W5100_GetIMR( void ) { return W5100_W51_Read( 0x0016 ); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write the retry time register
 * \param time the value to be written to the regster
 */
static void W5100_SetRetryTime( uint16 time) { W5100_W51_Write16( 0x0017, time ); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the retry tie register
 * \returns the value read from the register
 */
static uint16 W5100_GetRetryTime( void ) { return W5100_W51_Read16( 0x0017 ); }
/* ------------------------------------------------------------------------ */
/**
 * \breif write a value to the retry count register
 * \param count the value to be written to the register
 */
static void W5100_SetRetryCount( uint8 count ) { W5100_W51_Write( 0x0019, count); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the contents of the retry count register
 * \returns the value read from teh register
 */
static uint8 W5100_GetRetryCount( void ) { return W5100_W51_Read( 0x0019 ); }
/* ------------------------------------------------------------------------ */
/**
 * \brief write a value to the Rx mem size register
 * \param size the value to be written to the register
 */
static void W5100_SetRxMemSize( uint8 size ) { W5100_W51_Write( 0x1A, size ); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read the contents of hte rx mem size register
 * \returns the value read from teh register
 */
static uint8 W5100_GetRxMemSize( void ) { return W5100_W51_Read(0x1A); }
/* ------------------------------------------------------------------------ */
/**
 * \brief write a value to the tx mem size register
 * \param size The value to be written to the register
 */
static void W5100_SetTxMemSize( uint8 size ) { W5100_W51_Write( 0x1B, size); }
/* ------------------------------------------------------------------------ */
/**
 * \brief read the contents of the tx mem size register
 * \returns the value read from the register
 */
static uint8 W5100_GetTxMemSize( void ) { return W5100_W51_Read( 0x1B);}
/* ------------------------------------------------------------------------ */
/**
 * \brief write a value to the unreachable IP register
 * \param ip the value to be written to the register
 */
static void W5100_SetUnreachableIP( uint32 ip) { W5100_W51_SetIP( 0x2A, ip); }
/* ------------------------------------------------------------------------ */
/**
 * \brief read the contents of the unreachable IP register
 * \returns the value read from teh register
 */
static uint32 W5100_GetUnreachableIP( void ) { return W5100_W51_GetIP( 0x2A ); }
/* ------------------------------------------------------------------------ */
/**
 * \brief write a value to the unreachable port register
 * \param port the value to be written to the register
 */
static void W5100_SetUnreachablePort( uint16 port) { W5100_W51_Write16(0x2E, port); }
/* ------------------------------------------------------------------------ */
/**
 * \brief read the contents of the unreachable port register
 * \returns the value read from the register
 */
static uint16 W5100_GetUnreachablePort( void ) { return W5100_W51_Read16(0x2E); }
/* ======================================================================== */
/* End Section */
#endif
/* ======================================================================== */
/* Socket Registers */
#if (1)
/**
 * \def W5100_SOCKET BASE(s)
 * \brief Convert the socket number to a base address within the W5100
 */
#define W5100_SOCKET_BASE(s)           ((s<<8)+0x0400)
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket mode register
 * \param socket the socket number for the addressed register
 * \param mode the value to be written to the register
 */
static void W5100_SetSocketMode(uint8 socket, uint8 mode)
{ W5100_W51_Write(W5100_SOCKET_BASE(socket), mode); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket mode register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint8 W5100_GetSocketMode(uint8 socket)
{ return W5100_W51_Read(W5100_SOCKET_BASE(socket)); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket command register
 * \param socket the socket number for the addressed register
 * \param cmd the value to be written to the register
 */
static void W5100_SetSocketCommand(uint8 socket, uint8 cmd)
{ W5100_W51_Write(W5100_SOCKET_BASE(socket)+1, cmd); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket command register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint8 W5100_GetSocketCommand(uint8 socket)
{ return W5100_W51_Read(W5100_SOCKET_BASE(socket)+1); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket interrupt register
 * \param socket the socket number for the addressed register
 * \param ir the value to be written to the register
 */
static void W5100_SetSocketIR(uint8 socket, uint8 ir)
{ W5100_W51_Write(W5100_SOCKET_BASE(socket)+2,ir); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket interrupt register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint8 W5100_GetSocketIR(uint8 socket)
{ return W5100_W51_Read(W5100_SOCKET_BASE(socket)+2); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket status register
 * \param socket the socket number for the addressed register
 * \param status the value to be written to the register
 */
static void W5100_SetSocketStatus(uint8 socket, uint8 status)
{ W5100_W51_Write(W5100_SOCKET_BASE(socket)+3,status); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket status register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint8 W5100_GetSocketStatus(uint8 socket)
{ return W5100_W51_Read(W5100_SOCKET_BASE(socket)+3); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket source port register
 * \param socket the socket number for the addressed register
 * \param port the value to be written to the register
 */
static void W5100_SetSocketSourcePort(uint8 socket, uint16 port)
{ W5100_W51_Write16(W5100_SOCKET_BASE(socket)+4,port); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket source port register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint16 W5100_GetSocketSourcePort(uint8 socket)
{ return W5100_W51_Read16(W5100_SOCKET_BASE(socket)+4); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket Destination hardware address register
 * \param socket the socket number for the addressed register
 * \param *mac poitner to the array holding the values to be written to the register
 */
static void W5100_SetSocketDestMAC(uint8 socket, uint8* mac)
{ W5100_W51_SetMAC( W5100_SOCKET_BASE(socket)+6, mac); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket destination hardware address (MAC) register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static void W5100_GetSocketDestMAC(uint8 socket, uint8* mac)
{ W5100_W51_GetMAC( W5100_SOCKET_BASE(socket)+6,mac); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket Destination IP address register
 * \param socket the socket number for the addressed register
 * \param ip the value to be written to the register
 */
static void W5100_SetSocketDestIP(uint8 socket, uint32 ip)
{ W5100_W51_SetIP(W5100_SOCKET_BASE(socket)+12,ip); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket destination IP register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint32 W5100_GetSocketDestIP(uint8 socket )
{ return W5100_W51_GetIP(W5100_SOCKET_BASE(socket)+12); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket destination port register
 * \param socket the socket number for the addressed register
 * \param port the value to be written to the register
 */
static void W5100_SetSocketDestPort(uint8 socket, uint16 port)
{ W5100_W51_Write16( W5100_SOCKET_BASE(socket)+16, port); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket destination port register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint16 W5100_GetSocketDestPort(uint8 socket)
{ return W5100_W51_Read16( W5100_SOCKET_BASE(socket)+16 ); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket max segment size register
 * \param socket the socket number for the addressed register
 * \param size the value to be written to the register
 */
static void W5100_SetSocketMaxSegSize(uint8 socket, uint16 size)
{ W5100_W51_Write16(W5100_SOCKET_BASE(socket)+18,size); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket maximum segment size register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint16 W5100_GetSocketMaxSegSize(uint8 socket)
{ return W5100_W51_Read16(W5100_SOCKET_BASE(socket)+18); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket protocol register (IP RAW/MAC RAW mode)
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint16  W5100_GetSocketProto(uint8 socket)
{ return W5100_W51_Read(W5100_SOCKET_BASE(socket)+20); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket TOS register
 * \param socket the socket number for the addressed register
 * \param tos the value to be written to the register
 */
static void W5100_SetSocketTOS(uint8 socket, uint8 tos)
{ W5100_W51_Write(W5100_SOCKET_BASE(socket)+21, tos); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket TOS register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint8 W5100_GetSocketTOS(uint8 socket)
{ return W5100_W51_Read(W5100_SOCKET_BASE(socket)+21); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket Time-To_Live (TTL) register
 * \param socket the socket number for the addressed register
 * \param ttl the value to be written to the register
 */
static void W5100_SetSocketTTL(uint8 socket, uint8 ttl)
{ W5100_W51_Write(W5100_SOCKET_BASE(socket)+22,ttl); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket time-to-live (TTL) register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint8 W5100_GetSocketTTL(uint8 socket)
{ return W5100_W51_Read(W5100_SOCKET_BASE(socket)+22); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket tx free size register
 * \param socket the socket number for the addressed register
 * \param size the value to be written to the register
 */
static void W5100_SetSocketTxFree( uint8 socket, uint16 size)
{ W5100_W51_Write16(W5100_SOCKET_BASE(socket)+0x20,size); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket Tx free size register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint16 W5100_GetSocketTxFree(uint8 socket)
{ return W5100_W51_Read16(W5100_SOCKET_BASE(socket)+0x20); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket tx read pointer register
 * \param socket the socket number for the addressed register
 * \param ptr the value to be written to the register
 */
static void W5100_SetSocketTxReadPtr(uint8 socket, uint16 ptr)
{ W5100_W51_Write16(W5100_SOCKET_BASE(socket)+0x22,ptr); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket Tx buffer read pointer register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint16 W5100_GetSocketTxReadPtr(uint8 socket)
{ return W5100_W51_Read16(W5100_SOCKET_BASE(socket)+0x22); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket tx write pointer register
 * \param socket the socket number for the addressed register
 * \param ptr the value to be written to the register
 */
static void W5100_SetSocketTxWritePtr(uint8 socket, uint16 ptr)
{ W5100_W51_Write16(W5100_SOCKET_BASE(socket)+0x24,ptr); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket tx buffer write pointer register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint16 W5100_GetSocketTxWritePtr(uint8 socket)
{ return W5100_W51_Read16(W5100_SOCKET_BASE(socket)+0x24); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket rx received data size register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint16 W5100_GetSocketRxSize(uint8 socket)
{ return W5100_W51_Read16(W5100_SOCKET_BASE(socket)+0x26); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a value to the socket rx read pointer register
 * \param socket the socket number for the addressed register
 * \param ptr the value to be written to the register
 */
static void W5100_SetSocketRxReadPtr(uint8 socket, uint16 ptr)
{ W5100_W51_Write16(W5100_SOCKET_BASE(socket)+0x28,ptr); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Read a value from the socket rx buffer read pointer register
 * \param socket the socket number for the addressed register
 * \returns the value read from the register
 */
static uint16 W5100_GetSocketRxReadPtr(uint8 socket)
{ return W5100_W51_Read16(W5100_SOCKET_BASE(socket)+0x28); }
/* ------------------------------------------------------------------------ */
/**
 * \brief Write a command to the socket command register and wait for completion
 * \param socket the addressed socket for the command
 * \param cmd the command to execute
 */
static uint32 W5100_ExecuteSocketCommand( uint8 socket, int cmd)
{
	uint32 timeout;
	timeout = 0;
	
	W5100_SetSocketCommand(socket,cmd);
	while ( ( W5100_GetSocketCommand(socket) ) && (timeout < 125))
	{
		++timeout;
		CyDelay(1);
	}
	return( timeout );
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Execute a safe read fo the Tx buffer free size register
 * \param socket the addressed socket to read the available tx buffer
 * \returns the number of bytes available in the transmit buffer for the socket
 */
static uint16 W5100_GetTxFreeSize( uint8 socket )
{
	uint16 first, second;
	
	first = 0;
	second = 0;
	do {
		first = W5100_GetSocketTxFree( socket );
		if (first != 0) {
			second = W5100_GetSocketTxFree( socket );
		}
	}
	while (first != second );
	
	return( second );
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Execute a safe read of the received data size
 * \param socket the addressed socketfrom which the data will be read
 * \returns the number of bytes waiting in the recieve buffer of the socket
 */
static uint16 W5100_GetRxSize( uint8 socket )
{
	uint16 first, second;
	
	first = 0;
	second = 0;
	do {
		first = W5100_GetSocketRxSize( socket );
		if (first != 0) {
			second = W5100_GetSocketRxSize( socket );
		}
	}
	while (first != second );
	
	return second;
}
/* ------------------------------------------------------------------------ */

// END Socket Register access section
#endif
/* ======================================================================== */
/* W5100 Data Buffer Memory access primitives */
#if (1)
/* ------------------------------------------------------------------------ */
/**
 * \brief Transfer data from a local data buffer to the chip Tx fifo
 * \param socket the socket buffer to which the data will be written
 * \param offset The offest in to the socket buffer
 * \param *buffer pointer to the local buffer to copy to the transmit fifo
 * \param length the number of bytes to be copied to the transmit fifo
 */
static void W5100_ProcessTxData(uint8 socket, uint16 offset, uint8* buffer, uint16 length)
{
	uint16 addr;
	uint16 base;
	uint16 PointerOffset;
	uint16 size;
	
	/*
	 * Read the offset pointer, and calculate the base address for the start of write
	 * in to the chip buffers.  Written data will be offset by the offset byte count
	 * specified in the parameters.
	 */
	base = W5100_GetSocketTxWritePtr(socket) + offset;
	PointerOffset = (base & 0x07FF);
	addr = PointerOffset + W5100_SOCKET_TX_BASE(socket);
	/* calculate the number of bytes from the pointer to the end of the buffer */
	size = 0x0800 - PointerOffset;
	
	/*
	 * is there enough space to write the complete packet
	 * or, shoudl the data be split in to two writes.
	 */
	if ( length > size) {
		/*
		 * There is more data to send than will fit from the pointer to the end
		 * of the memory, so we will write the data in two chunks.  First, write the
		 * data from the pointer to the end of the buffer, then write the portion
		 * remaining to the start (base ptr) of the socket buffer
		 */
		W5100_W51_WriteBlock(addr, buffer, size);
		W5100_W51_WriteBlock(W5100_SOCKET_TX_BASE(socket), &buffer[size], length - size);
	}
	else {
		/* 
		 * there is enough available space from the write point to the end of buffer
		 * to hold the whole packe of information, so just write it to the memory
		 */
		W5100_W51_WriteBlock(addr,buffer,length);
	}
	/* move the write pointer */
	base += length;
	/* 
	 * Store the new write pointer so that the device knows that there is data waiting
	 * to be transmitted over the link.
	 */
	W5100_SetSocketTxWritePtr(socket, base);
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Transfer data from the chip receive buffer to a local buffer
 * \param socket the socket buffer from which the data will be read
 * \param offset The offest in to the socket buffer
 * \param *buffer pointer to the local buffer to hold the receive fifo data
 * \param length the max number of bytes to be copied to the local buffer
 * \param flags Flag settings to control read fifo options (lookahead)
 */
static void W5100_ProcessRxData(uint8 socket, uint16 offset, uint8* buffer, uint16 length, uint8 flags)
{
	uint16 addr;
	uint16 base;
	uint16 PointerOffset;
	uint16 size;
	
	/*
	 * Read the offset pointer, and calculate the base address for the start of read
	 * in to the chip buffers.  The data read will be offset by the offset byte count
	 * specified in the parameters.
	 */
	base = W5100_GetSocketRxReadPtr(socket) + offset;
	PointerOffset = (base & 0x07FF);
	addr = PointerOffset + W5100_SOCKET_RX_BASE(socket);
	/* calculate the number of bytes from the pointer to the end of the buffer */
	size = 0x0800 - PointerOffset;
	/*
	 * is there enough space to read the complete packet
	 * or, should the data be split in to two reads.
	 */
	if ( length > size) {
		/*
		 * There is more data to send than will fit from the pointer to the end
		 * of the memory, so we will write the data in two chunks.  First, write the
		 * data from the pointer to the end of the buffer, then write the portion
		 * remaining to the start (base ptr) of the socket buffer
		 */
		W5100_W51_ReadBlock(addr, buffer, size);
		W5100_W51_ReadBlock(W5100_SOCKET_RX_BASE(socket), &buffer[size], length - size);
	}
	else {
		/* 
		 * there is enough available space from the write point to the end of buffer
		 * to hold the whole packe of information, so just write it to the memory
		 */
		W5100_W51_ReadBlock(addr,buffer,length);
	}
	if ( (flags & 0x01) == 0 ) { /* V1.1: Added ==0 condition to lookahead flag check */
		/* move the write pointer */
		base += length;
		/* 
		 * Store the new write pointer so that the device knows that there is data waiting
		 * to be transmitted over the link.
		 */
		W5100_SetSocketRxReadPtr(socket, base);
	}
}
#endif
/* ======================================================================== */
/* Driver Initialization */
#if (1)
/* ------------------------------------------------------------------------ */
void
W5100_Init(uint8* mac, uint32 ip, uint32 subnet, uint32 gateway)
{
	/*
	 * Initialize the device and the address configuration, then
	 * execute socket memory initialization for the local allocation
	 * table.
	 */
	int index;
	
	CyDelay( 250 ); /* Fixed Boot Delay or chip inintialization will fail */
	
	/* Reset the device */
	W5100_SetMode(0x80); /* Send Reset Command */
	/* Wait for the chip initialization delay */
	CyDelay( 10 );
	
	/* Close all of the socket, and clear the memory to make them available. */
	for(index=0;index<4;++index) {
		W5100_SocketClose( index );
	}
	/* Write the default configruation for memory size to the device */
	W5100_SetTxMemSize(0x55); /* 2K each */
	W5100_SetRxMemSize(0x55); /* 2K Each */
	/* Set device gateway address */
	W5100_SetGatewayAddress(gateway);
	W5100_SetSubnetMask( subnet );
	/* Store the subnet mask for later use, for ERRATA fix */
	W5100_SubnetMask = subnet;
	/* store the device mac */
	W5100_SetSourceMAC( mac );
	/* store the IP address */
	W5100_SetSourceIP( ip );
	/* clear pending interrupts */
	W5100_SetIR(0xFF);
	/* clear the subnet mask register (W5100 Errata Fix) */
	W5100_SetSubnetMask( 0 );
}
/* ------------------------------------------------------------------------ */
void
W5100_Start( void )
{
	uint32 ip;
	uint32 gateway;
	uint32 sub;
	
	/* Check for SPI initialization */
	if (SPI_initVar == 0) {
		/*
		 * the hardware has not yet been initialized.  Before anything can be
		 * initialized within the driver, the external hardware must be started
		 */
		SPI_Start();
	}
		
	/* wait for power on PLL Lock */
	CyDelay( 10 );
	/* V1.2 new -- Parse MAC Address string */
	if ( W5100_ParseMAC("00-DE-AD-BE-EF-00", &W5100_MAC[0]) == CYRET_BAD_DATA) {
		W5100_MAC[0] = 0;
		W5100_MAC[1] = 0xDE;
		W5100_MAC[2] = 0xAD;
		W5100_MAC[3] = 0xC0;
		W5100_MAC[4] = 0xDE;
		W5100_MAC[5] = 0;
	}
	/* END of V1.2 Update ------ */
	ip = W5100_ParseIP("192.168.1.101");
	sub = W5100_ParseIP("255.255.255.0");
	gateway = W5100_ParseIP("192.168.1.1");
	/* Initialize the device with the default data */
	W5100_Init( &W5100_MAC[0], ip, sub, gateway  );
}
/* ------------------------------------------------------------------------ */
uint32
W5100_ParseIP( const char* ipString )
{
	/*
	 * Parse a human readable string in to a IP address usable by the hardare
	 */
	int index;
	char digit[5];
	uint8 ip[4];
	int counter;
	int dindex;
	
	index = 0;
	dindex = 0;
	counter = 0;
	
	while ( (counter < 4) && ((unsigned int)index < strlen(ipString) ) ) {
		if ( (ipString[index] >= '0' ) && (ipString[index] <= '9') ) {
			if (dindex > 3) return( 0xFFFFFFFF );
			digit[dindex++] = ipString[index];
			
		}
		
		if ( (ipString[index] == '.') || (ipString[index+1] == 0) ) {
			digit[dindex] = 0;
			dindex = 0;
			/* convert the value and store in the buffer */
			ip[counter] = 0;
			while (digit[dindex] != 0) {
				ip[counter] = (ip[counter]*10) + (digit[dindex]-'0');
				++dindex;
				// ip[counter] = ...
			}
			/* reset the digit index to start accumulating digits again*/
			dindex = 0;
			/* move to the next address byte */
			++counter;
		}
		++index;
	}
	
	if (counter != 4) {
		return( 0xFFFFFFFF );
	}
	else {
		return( W5100_IPADDRESS(ip[0], ip[1], ip[2], ip[3]) );
	}
}
/* ------------------------------------------------------------------------ */
/**
 * /brief Parse a MAC Address string in to a 6-byte mac address
 * /param *macString Pointer to the ASCII-Z String containing the MAC address
 * /param *mac Pointer to the 6-byte array to hold the output mac addres
 */
cystatus W5100_ParseMAC(const char *macString, uint8 *mac)
{
	/* 
	 * a mac address is specified as a string of 6 hex bytes with
	 * dashes ('-') seperating the bytes.  An invalidly formed
	 * address will only process the values up the error and return BAD_DATA
	 * otherwise, SUCESS is returned.
	 */
	int digit;
	int index;
	cystatus result;
	
	result = CYRET_SUCCESS;
	index = 0;
	for(digit = 0;(digit<6) && (result == CYRET_SUCCESS)&&(macString[index] != 0);++digit) {
		// process the first nibble
		if (W5100_ISXDIGIT(macString[index]) ) {
			mac[digit] = W5100_HEX2BIN(macString[index++]);
			mac[digit] <<= 4;
			if (W5100_ISXDIGIT(macString[index])) {
				mac[digit] += W5100_HEX2BIN(macString[index++]);
				/*
				 * now for digits other than digit 5 (the last one) look for
				 * the dash seperator.  If there is no dash, return bad data
				 */
				if (digit<5) {
					if (macString[index]!='-') {
						result = CYRET_BAD_DATA;
					}
					++index; // move conversion pointer to the next value
				}
			}
			else {
				result = CYRET_BAD_DATA;
			}
		}
		else {
			result = CYRET_BAD_DATA;
		}
	}
	return( result );
}
/* ------------------------------------------------------------------------ */
void W5100_StringMAC(char *macString)
{
	uint8 mac[6];
	int digit;
	int index;
	
	/*
	 * first read the MAC address from the chip
	 * and inintialize some locals so that the
	 * string formater will function properly
	 */
	W5100_GetSourceMAC( &mac[0] );
	index = 0;
	for(digit=0;digit<6;++digit) {
		// convert the first nibble
		macString[index++] = W5100_BIN2HEX(((mac[digit]>>4)&0x0F));
		macString[index++] = W5100_BIN2HEX((mac[digit]&0x0F));
		if (digit<5) {
			macString[index++] = '-';
		}
		else {
			macString[index] = 0;
		}
	}
}
/* ------------------------------------------------------------------------ */
void W5100_StringIP( char *ipString )
{
	uint32 ip;
	uint8 *ipBytes;
	int index;
	int digit;
	int work, temp;
	
	ip = W5100_GetSourceIP();
	ipBytes = (uint8*)&ip;
	index = 0;
	for(digit=0;digit<4;++digit) {
		work = ipBytes[digit];
		if (work >= 100) {
			temp = work/100;
			work -= (temp*100);
			ipString[index++] = '0' + temp;
		}
		if (work >= 10) {
			temp = work /10;
			work -= (temp*10);
			ipString[index++] = '0'+temp;
		}
		ipString[index++] = '0'+work;
		if (digit <3) {
			ipString[index++] = '.';
		}
		else {
			ipString[index] = 0;
		}
	}
}

/* ------------------------------------------------------------------------ */
uint8
W5100_SetIP( uint32 ip )
{
	uint8 ok = 0;
	
	if ( ( ip != 0) && (ip != 0xFFFFFFFF) ) {
		W5100_SetSourceIP( ip );
		ok = 0xFF;
	}
	
	return ok;
}
/* ------------------------------------------------------------------------ */
uint32
W5100_GetIP( void )
{
	return W5100_GetSourceIP();
}
/* ------------------------------------------------------------------------ */
void
W5100_SetMAC( uint8* mac )
{
	W5100_SetSourceMAC( mac );
}
/* ------------------------------------------------------------------------ */
void
W5100_GetMAC( uint8* mac )
{
	W5100_GetSourceMAC( mac );
}
#endif
/* ======================================================================== */
/* Socket Controls */
#if (1)
/* ------------------------------------------------------------------------ */
uint8
W5100_SocketOpen( uint8 Protocol, uint16 port, uint8 flags )
{
	uint8 socket;
	int index;
	
	/*
	 * This function will look for the first available socket for allocation
	 * using the defined protocol.  The socket configuration will then be
	 * initialized and saved in the allocation table. If the requested socket
	 * can not be initialized, 0xFF is returned
	 */
	socket = 0xFF;
	/* find the first available socket to open, and how much memory is available */
	for( index = 0;index<4;++index) {
		if ( (socket == 0xFF) && (W5100_SocketConfig[index].Protocol == 0) ) {
			/*
			 * Since the W5100 does not support MAC mode commucications, check to see
			 * if the socket that is free was socket 0.  If the mac protocol was
			 * selected, ignore socket 0
			 */
			if ( (Protocol != W5100_PROTO_MAC) || (index != 0) ) {
				/* store this socket number for allocation */
				socket = index;
			}
		}
	}
	if (socket != 0xFF ) {
		/* Store the socket information in the allocation table */
		W5100_SocketConfig[socket].Protocol = Protocol;
		W5100_SocketConfig[socket].SocketFlags = flags;
		W5100_SocketConfig[socket].SourcePort = port;
		W5100_SocketConfig[socket].ServerFlag = 0;
		/* Send the socket open with the correct protocol information */
		W5100_SetSocketSourcePort( socket, port );
		W5100_SetSocketMode( socket, Protocol | flags );
		W5100_ExecuteSocketCommand( socket, 1 );
	}
	return socket;
}
/* ------------------------------------------------------------------------ */
void
W5100_SocketClose( uint8 socket )
{
	/*
	 * Set the socket allocation to empty, and issue the close command
	 * to the socket
	 */
	if (socket < 4) {
		/* clear the socket memory allocations */
		W5100_SocketConfig[socket].Protocol = 0;
		W5100_SocketConfig[socket].SocketFlags = 0;
		/* close the socket */
		W5100_ExecuteSocketCommand( socket, 0x10 );
		/* Clear pending Interrupts */
		W5100_SetSocketIR( socket, 0xFF);
	}
}
/* ------------------------------------------------------------------------ */
uint8
W5100_SocketProcessConnections( uint8 socket )
{
	uint8 status;
	
	status = W5100_GetSocketStatus(socket);
	/* has a connection termination been requested by remote system */
	if (status == 0x1C) {
		/* Close the socket on this end */
		W5100_SocketClose(socket);
	}
	return (W5100_GetSocketStatus(socket) == 0x00);
}
/* ------------------------------------------------------------------------ */
uint8
W5100_SocketEstablished( uint8 socket )
{
	return (W5100_GetSocketStatus( socket ) == 0x17);
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Transmit a SEND operation over a socket
 * \param socket the socket to which the send command will be sent
 */
static void
W5100_SocketSend(uint8 socket )
{
	uint8 ir;
	
	/* initialize the subnet mask register : ERRATA FIX */
	W5100_SetSubnetMask( W5100_SubnetMask );
	/* Issue the SEND command */
	W5100_ExecuteSocketCommand( socket, 0x20 );
	/* wait for the SEND to complete, or for a timeout */
	ir = W5100_GetSocketIR( socket );
	/* while SEND is not done, and the socket hasnot timed out or been dsconnected */
	while ( ((ir & 0x10) == 0) && (!(ir&0x0A)) ) {
		CyDelay(1);
		ir = W5100_GetSocketIR( socket );
		
	}
	/* clear the SEND_OK flag from the register */
	W5100_SetSocketIR( socket, 0x10 );
	/* reset the subnet mask : ERRATA FIX */
	W5100_SetSubnetMask( 0 );
}
/* ------------------------------------------------------------------------ */
/**
 * \brief Execute a SEND without an ARP.
 * \param socket the socket to which the SEND will be executed.
 */
static void
W5100_SocketSendMac(uint8 socket )
{
	uint8 ir;
	
	/* initialize the subnet mask register : ERRATA FIX */
	W5100_SetSubnetMask( W5100_SubnetMask );
	/* Issue the SEND command */
	W5100_ExecuteSocketCommand( socket, 0x21 );
	/* wait for the SEND to complete, or for a timeout */
	ir = W5100_GetSocketIR( socket );
	/* while SEND is not done, and the socket hasnot timed out or been dsconnected */
	while ( ((ir & 0x10) == 0) && (!(ir&0x0A)) ) {
		CyDelay(1);
		ir = W5100_GetSocketIR( socket );
		
	}
	/* clear the SEND_OK flag from the register */
	W5100_SetSocketIR( socket, 0x10 );
	/* reset the subnet mask : ERRATA FIX */
	W5100_SetSubnetMask( 0 );
}
/* ------------------------------------------------------------------------ */
uint16
W5100_SocketRxDataWaiting( uint8 socket )
{
	return W5100_GetRxSize( socket );
}
#endif
/* ======================================================================== */
/* TCP/IP */
#if (1)
/* ------------------------------------------------------------------------ */
uint8
W5100_TcpOpen(uint16 port)
{
	return W5100_SocketOpen(W5100_PROTO_TCP, port, 0);
}
/* ------------------------------------------------------------------------ */
void
W5100_TcpStartServer( uint8 socket )
{
	/*
	 * Start a open socket listening for a connection
	 */
	/* was this a valid socket? */
	if (socket < 4) {
		/* Execute the listen command */
		W5100_ExecuteSocketCommand( socket, 0x02 );
	}
}
/* ------------------------------------------------------------------------ */
void
W5100_TcpStartServerWait( uint8 socket )
{
	W5100_TcpStartServer(socket);
	/* wait for socket establishment */
	while ( !W5100_SocketEstablished(socket) ) {
		CyDelay(1);
	}
}
/* ------------------------------------------------------------------------ */
void
W5100_TcpConnect( uint8 socket, uint32 ip, uint16 port )
{
	uint32 timeout;
	uint8 ir;
	
	if ( (ip != 0xFFFFFFFF) && (ip != 0) ) {
		W5100_SetSocketDestIP( socket, ip );
		W5100_SetSocketDestPort( socket, port );
		/* set socket subnet mask */
		W5100_SetSubnetMask( W5100_SubnetMask );
		W5100_ExecuteSocketCommand( socket, 0x04);
		/* wait for the socket connection to the remote host is established */
		while ( (!W5100_SocketEstablished(socket))  && (timeout < 3000) ) {
			CyDelay(1);
			++timeout;
			ir = W5100_GetSocketIR( socket );
			if ( (ir & 0x08) != 0 ) {
				/* internal chip timeout occured */
				timeout = 3000;
			}
		}
		/* clear the Subnet mask register */
		W5100_SetSubnetMask(0);
	}
}
/* ------------------------------------------------------------------------ */
uint8
W5100_TcpConnected( uint8 socket )
{ return W5100_SocketEstablished(socket); }
/* ------------------------------------------------------------------------ */
void
W5100_TcpDisconnect( uint8 socket )
{
	W5100_ExecuteSocketCommand(socket, 0x08);
}
/* ------------------------------------------------------------------------ */
uint16
W5100_TcpSend(uint8 socket, uint8* buffer, uint16 len)
{
	uint16 TxSize;
	uint16 FreeSpace;
	uint8 status;
	
	TxSize =  (len > 0x0800) ? 0x0800 : len;
	/* check the connection status, and protocol of the socket */
	status = W5100_GetSocketStatus(socket);
	if ( ( status == 0x17) && (W5100_SocketConfig[socket].Protocol == W5100_PROTO_TCP) ) {
		/* 
		 * The socket was open with the correct protocol and is connected to
		 * a valid remote system. In order to send the requested packet data,
		 * first, we must wait for the available buffer memory to be free
		 * in the transmit buffer fifo.
		 */
		FreeSpace = 0;
		status = 0x17;
		while ( (FreeSpace < TxSize) && ( (status == 0x17) && (status != 0x1C) ) ) {
			FreeSpace = W5100_GetTxFreeSize( socket );
			status = W5100_GetSocketStatus( socket );
		}
		/*
		 * Write the data buffer to the chip Tx Buffer, then issue
		 * the chip SEND command
		 */
		W5100_ProcessTxData(socket, 0, buffer, TxSize);
		W5100_SocketSend( socket );
	}
	else {
		/*
		 * the socket was not establised, or there was an error, so clear
		 * the TxSize to signal that no data has bee sent
		 */
		TxSize = 0;
	}
	return TxSize;
}
/* ------------------------------------------------------------------------ */
uint16
W5100_TcpReceive( uint8 socket, uint8* buffer, uint16 length )
{
	uint16 RxSize;

	/*
	 * First thing to do is to check to see if the socket is connected
	 * and that there is data waiting
	 */
//	RxSize = 0;

//	if ( W5100_GetSocketStatus( socket ) == 0x17) {
		/*
		 * read the number of waiting bytes in the buffer memory
		 * but, clip the length of data read to the requested
		 * length of data.
		 */
		RxSize = W5100_GetRxSize( socket );
		RxSize = (RxSize > length) ? length : RxSize;
		/* If there was waiting data, read it from the buffer */
		if (RxSize > 0) {
			W5100_ProcessRxData( socket, 0, buffer, RxSize, 0);
			/* 
			 * after reading the buffer data, send the receive command
			 * to the socket so that the W5100 completes the read
			 */
			W5100_ExecuteSocketCommand(socket, 0x40);
		}
//	}
	
	/* return the number of read bytes from the buffer memory */
	return RxSize;
}
/* ------------------------------------------------------------------------ */
void
W5100_TcpPrint( uint8 socket, const char* str )
{
	W5100_TcpSend(socket, (uint8*)str, strlen(str));
}
#endif
/* ======================================================================== */
/* UDP */
#if (1)
/* ------------------------------------------------------------------------ */
uint8
W5100_UdpOpen(uint16 port)
{
	return W5100_SocketOpen(W5100_PROTO_UDP, port, 0);
}
/* ------------------------------------------------------------------------ */	
uint16
W5100_UdpSend(uint8 socket, uint32 ip, uint16 port, uint8* buffer, uint16 length)
{
	uint16 TxSize;
	
	/*
	 * Transmit a buffer of data to a specified remote system using UDP.
	 */
	if (W5100_GetSocketStatus(socket) == 0x17) {
		/*
		 * The socket has been established, so wait for available
		 * room in the transmit buffer of the socket, but, trim the
		 * transmitted data length to no more than the available
		 * buffer size in the device. (2K)
		 */
		TxSize = (length > 0x0800) ? 0x0800 : length;
		if ( (ip != 0) && (ip != 0xFFFFFFFF) ) {
			/*
			 * Store the destination IP and port in the chip
			 * socket registers.
			 */
			W5100_SetSocketDestIP( socket, ip );
			W5100_SetSocketDestPort( socket, port );
			/*
			 * process the transmission buffer, and write it in to
			 * the chip buffer memory.
			 */
			W5100_ProcessTxData(socket,0,buffer,TxSize);
			/*
			 * Issue the send command to transmit the buffer.
			 */
			W5100_SocketSend( socket ); 
		}
		else {
			/* an invalid IP address was issued, so return that no data was sent. */
			TxSize = 0;
		}
	}
	return TxSize;
}
/* ------------------------------------------------------------------------ */
uint16
W5100_UdpReceive(uint8 socket, uint32 *ip, uint16 *port, uint8* buffer, uint16 length)
{
	uint16 RxSize;
	uint16 PacketSize;
	uint8  PacketHeader[8];
	
	/*
	 * First thing to do is to check to see if the socket is connected
	 * and that there is data waiting
	 */
	RxSize = 0;
	if (W5100_GetSocketStatus( socket ) == 0x17) {
		/*
		 * read the number of waiting bytes in the buffer memory
		 * but, clip the length of data read to the requested
		 * length of data.
		 */
		RxSize = W5100_GetRxSize( socket );
		RxSize = (RxSize > length+8) ? length : RxSize;
		/* If there was waiting data, read it from the buffer */
		if (RxSize > 0) {
			/*
			 * the UDP packet is stored in the buffer memory as an 8-byte
			 * packet header followed by the packet data.
			*/
			W5100_ProcessRxData( socket, 0, &PacketHeader[0], 8, 0);
			/*
			 * The packet header contains the 4-byte IP address followed by the
			 * 2-byte port number and the 2-byte packet data length
			 */
			*ip = W5100_IPADDRESS(PacketHeader[0], PacketHeader[1], PacketHeader[2], PacketHeader[3]);
			*port = (uint16)((PacketHeader[4]<<8)&0xFF00) | (PacketHeader[5]&0x00FF);
			PacketSize = (uint16)((PacketHeader[6]<<8)&0xFF00) | (PacketHeader[7]&0x00FF);
			/*
			 * Check to make sure that the packet data has been received completely
			 */
			if (RxSize >= (PacketSize+8) ) {
			
				W5100_ProcessRxData( socket, 8, buffer, PacketSize, 0);
				/* 
				 * after reading the buffer data, send the receive command
				 * to the socket so that the W5100 completes the read
				 */
				W5100_ExecuteSocketCommand(socket, 0x40);
				RxSize = PacketSize;
			}
			else {
				RxSize = 8;
			}
		}
	}
	
	/* return the number of read bytes from the buffer memory */
	return RxSize;
}
#endif
/* ======================================================================== */
/* IGMP */

/* ======================================================================== */
/* DHCP */

/* ======================================================================== */
/* HTTP - Web Server */

/* [] END OF FILE */
/** @} */
