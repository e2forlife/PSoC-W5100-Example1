/* ========================================
 *
 * Copyright YOUR COMPANY, THE YEAR
 * All Rights Reserved
 * UNPUBLISHED, LICENSED SOFTWARE.
 *
 * CONFIDENTIAL AND PROPRIETARY INFORMATION
 * WHICH IS THE PROPERTY OF your company.
 *
 * ========================================
*/
#include <project.h>

int main()
{
	uint8 tcpSocket;
	/*
	 * First the SPI perepheral must be initialized so that the
	 * W5100 driver has hardware to use for the chip initialization
	 * that will occur next
	 */
	SPI_Start();
  	/*
	 * The next section initialized the W5100 to default settings
	 * setup in the component confiuration dialog.
	 */
	W5100_Start();
    for(;;)
    {
        /*
		 * Now that the W5100 is initialized and configured,
		 * open a socket, then start a server and wait for connections
		 */
		tcpSocket = W5100_TcpOpen( 23 );
		/*
		 * If the socket was opened the socket number will be 0 - 3
		 * to reflect the socket that was used within the W5100
		 */
		if (tcpSocket < 4) {
			/* Wait for server connections */
			W5100_TcpStartServerWait( tcpSocket );
			/* Send the string to the remote terminal */
			W5100_TcpPrint(tcpSocket,"\x1b[2JHello From E2ForLife.com\r\n");
			/* Wait for string to transmit over the network before closing */
			CyDelay(10);
			/* Disconnect from client (drop connection) */
			W5100_TcpDisconnect(tcpSocket);
			/* Close the socket */
			CyDelay(50);
			W5100_SocketClose(tcpSocket);
		}
    }
}

/* [] END OF FILE */
