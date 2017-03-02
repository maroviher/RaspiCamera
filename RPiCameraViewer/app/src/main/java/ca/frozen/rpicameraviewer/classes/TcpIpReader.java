// Copyright © 2016 Shawn Baker using the MIT License.
package ca.frozen.rpicameraviewer.classes;

import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.Socket;

public class TcpIpReader extends RawH264Reader
{
	// local constants
	private final static String TAG = "TcpIpReader";
	private final static int SOCKET_TIMEOUT = 200;

	// instance variables
	private Socket socket = null;
	private InputStream inputStream = null;

	//******************************************************************************
	// TcpIpReader
	//******************************************************************************
	public TcpIpReader(Source source)
	{
		super(source);
		try
		{
			socket = getConnection(source.address, source.port);
			inputStream = socket.getInputStream();
		}
		catch (Exception ex) {}
	}

	//******************************************************************************
	// read
	//******************************************************************************
	public int read(byte[] buffer)
	{
		try
		{
			return (inputStream != null) ? inputStream.read(buffer) : 0;
		}
		catch (IOException ex)
		{
			return 0;
		}
	}

	public Boolean read_exact(byte[] buffer, int cnt)
	{
		if(inputStream == null)
			return false;
		try
		{
			int iToRead = cnt;
			int offset = 0;
			int read = 0;
			while(iToRead > 0)
			{
				read = inputStream.read(buffer, offset, iToRead);
				iToRead -= read;
				offset += read;
			}
			return true;
		}
		catch (IOException ex)
		{
			return false;
		}
	}

	//******************************************************************************
	// isConnected
	//******************************************************************************
	public boolean isConnected()
	{
		return (socket != null) ? socket.isConnected() : false;
	}

	//******************************************************************************
	// close
	//******************************************************************************
	public void close()
	{
		if (inputStream != null)
		{
			try
			{
				inputStream.close();
			}
			catch (Exception ex) {}
			inputStream = null;
		}
		if (socket != null)
		{
			try
			{
				socket.close();
			}
			catch (Exception ex) {}
			socket = null;
		}
	}

	//******************************************************************************
	// getConnection
	//******************************************************************************
	public static Socket getConnection(String baseAddress, int port)
	{
		Socket socket = null;
		try
		{
			socket = new Socket();
			InetSocketAddress socketAddress = new InetSocketAddress(baseAddress, port);
			socket.connect(socketAddress, SOCKET_TIMEOUT);
		}
		catch (Exception ex)
		{
			socket = null;
		}
		return socket;
	}
}
