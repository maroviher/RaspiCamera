// Copyright © 2016 Shawn Baker using the MIT License.
package ca.frozen.rpicameraviewer.activities;

import android.app.Activity;
import android.content.Context;
import android.graphics.SurfaceTexture;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.media.MediaPlayer;
import android.os.Bundle;
import android.os.Handler;
import android.support.v4.app.Fragment;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.AlphaAnimation;
import android.view.animation.Animation;
import android.widget.Button;
import android.widget.TextView;

import java.io.InputStream;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;

import ca.frozen.rpicameraviewer.App;
import ca.frozen.rpicameraviewer.R;
import ca.frozen.rpicameraviewer.classes.Camera;
import ca.frozen.rpicameraviewer.classes.Source;
import ca.frozen.rpicameraviewer.classes.Utils;
import ca.frozen.library.views.ZoomPanTextureView;

import android.widget.SeekBar;

public class VideoFragment extends Fragment implements TextureView.SurfaceTextureListener, SeekBar.OnSeekBarChangeListener
{
	// public interfaces
	public interface OnFadeListener
	{
		void onStartFadeIn();
		void onStartFadeOut();
	}

	// public constants
	public final static String CAMERA = "camera";
	public final static String FULL_SCREEN = "full_screen";

	// local constants
	private final static String TAG = "VideoFragment";
	private final static float MIN_ZOOM = 0.1f;
	private final static float MAX_ZOOM = 10;
	private final static int FADEOUT_TIMEOUT = 3000;
	private final static int FADEOUT_ANIMATION_TIME = 500;
	private final static int FADEIN_ANIMATION_TIME = 400;

	// instance variables
	private Camera camera;
	private boolean fullScreen;
	private DecoderThread decoderThread;
	private ZoomPanTextureView textureView;
	private TextView textViewSS, messageView, framesView, textViewISO, textViewFPS;
	private Button button_SS, button_move_up, button_move_down, button_zoom_reset, button_move_left, button_move_right, button_mot, button_zoom_in, button_zoom_out;
	private Runnable fadeInRunner, fadeOutRunner, finishRunner, startVideoRunner;
	private Handler fadeInHandler, fadeOutHandler, finishHandler, startVideoHandler;
	private OnFadeListener fadeListener;

    private ArrayList<View> views_to_fade = new ArrayList<>();

	private SeekBar seekBar_iso, seekBar_ss;
	private int[] iso_map = {0, 100, 200, 400, 800, 1600};


	public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser)
	{
		switch(seekBar.getId())
		{
			case R.id.seekBar_iso:
				final int iso_seek = progress;
				getActivity().runOnUiThread(new Runnable()
				{
					public void run()
					{
                        textViewISO.setText(String.format("ISO=%d", iso_map[iso_seek]));
						if(decoderThread != null)
						{
							decoderThread.SetISO(iso_map[iso_seek]);
						}
					}
				});
				break;
			case R.id.seekBar_ss:
				final int ss_seek = progress * Integer.parseInt(button_SS.getText().toString());
				getActivity().runOnUiThread(new Runnable()
				{
					public void run()
					{
                        textViewSS.setText(String.format("ss=%d", ss_seek));
						if(decoderThread != null)
						{
							decoderThread.SetSS(ss_seek);
						}
					}
				});
				break;
		}
	}
	@Override
	public void onStartTrackingTouch(SeekBar seekBar) {
	}

	@Override
	public void onStopTrackingTouch(SeekBar seekBar) {
	}

	//******************************************************************************
	// newInstance
	//******************************************************************************
	public static VideoFragment newInstance(Camera camera, boolean fullScreen)
	{
		VideoFragment fragment = new VideoFragment();

		Bundle args = new Bundle();
		args.putParcelable(CAMERA, camera);
		args.putBoolean(FULL_SCREEN, fullScreen);
		fragment.setArguments(args);

		return fragment;
	}

	//******************************************************************************
	// onCreate
	//******************************************************************************
	@Override
	public void onCreate(Bundle savedInstanceState)
	{
		// configure the activity
		super.onCreate(savedInstanceState);


		// load the settings and cameras
		Utils.loadData();

		// get the parameters
		camera = getArguments().getParcelable(CAMERA);
		fullScreen = getArguments().getBoolean(FULL_SCREEN);

		// create the fade in handler and runnable
		fadeInHandler = new Handler();
		fadeInRunner = new Runnable()
		{
			@Override
			public void run()
			{
				Animation fadeInName = new AlphaAnimation(0, 1);
				fadeInName.setDuration(FADEIN_ANIMATION_TIME);
				fadeInName.setFillAfter(true);
				Animation fadeInSnapshot = new AlphaAnimation(0, 1);
				fadeInSnapshot.setDuration(FADEIN_ANIMATION_TIME);
				fadeInSnapshot.setFillAfter(true);
                for(View view : views_to_fade)
                    view.startAnimation(fadeInName);
				fadeListener.onStartFadeIn();
			}
		};

		// create the fade out handler and runnable
		fadeOutHandler = new Handler();
		fadeOutRunner = new Runnable()
		{
			@Override
			public void run()
			{
				Animation fadeOutName = new AlphaAnimation(1, 0);
				fadeOutName.setDuration(FADEOUT_ANIMATION_TIME);
				fadeOutName.setFillAfter(true);
				Animation fadeOutSnapshot = new AlphaAnimation(1, 0);
				fadeOutSnapshot.setDuration(FADEOUT_ANIMATION_TIME);
				fadeOutSnapshot.setFillAfter(true);

                for(View view : views_to_fade)
                    view.startAnimation(fadeOutName);
				fadeListener.onStartFadeOut();
			}
		};

		// create the finish handler and runnable
		finishHandler = new Handler();
		finishRunner = new Runnable()
		{
			@Override
			public void run()
			{
                if(getActivity() != null) {
                    MediaPlayer mp = MediaPlayer.create(getActivity(), R.raw.airbus_autopilot);
                    mp.start();
                    getActivity().finish();
                }
			}
		};

		// can do this only from this thread, not possible to move to decoderThread
		startVideoHandler = new Handler();
		startVideoRunner = new Runnable()
		{
			@Override
			public void run()
			{
				MediaFormat format = decoderThread.getMediaFormat();
				int videoWidth = format.getInteger(MediaFormat.KEY_WIDTH);
				int videoHeight = format.getInteger(MediaFormat.KEY_HEIGHT);
				textureView.setVideoSize(videoWidth, videoHeight);
			}
		};
	}

	//******************************************************************************
	// onCreateView
	//******************************************************************************
	@Override
	public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState)
	{
        //hide status bar
        View decorView = getActivity().getWindow().getDecorView();
        decorView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN);

		View view = inflater.inflate(R.layout.fragment_video, container, false);

        views_to_fade.add(button_zoom_reset = (Button)view.findViewById(R.id.button_zoom_reset));
        button_zoom_reset.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                decoderThread.Move('R');
            }
        });
        views_to_fade.add(button_move_left = (Button)view.findViewById(R.id.button_move_left));
        button_move_left.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                decoderThread.Move('l');
            }
        });
        views_to_fade.add(button_move_right = (Button)view.findViewById(R.id.button_move_right));
        button_move_right.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                decoderThread.Move('r');
            }
        });
        views_to_fade.add(button_move_up = (Button)view.findViewById(R.id.button_move_up));
        button_move_up.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                decoderThread.Move('u');
            }
        });
        views_to_fade.add(button_move_down = (Button)view.findViewById(R.id.button_move_down));
        button_move_down.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                decoderThread.Move('d');
            }
        });


        views_to_fade.add(textViewFPS = (TextView)view.findViewById(R.id.frames_cnt));
        views_to_fade.add(textViewSS = (TextView)view.findViewById(R.id.shutter_speed));

        views_to_fade.add(button_zoom_in = (Button)view.findViewById(R.id.button_zoom_in));
        button_zoom_in.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                decoderThread.ZoomIN();
            }
        });
        views_to_fade.add(button_zoom_out = (Button)view.findViewById(R.id.button_zoom_out));
        button_zoom_out.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                decoderThread.ZoomOUT();
            }
        });
        views_to_fade.add(button_SS = (Button)view.findViewById(R.id.button_ss));
        button_SS.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String bCap = button_SS.getText().toString();
                if(bCap.equals("1"))
                    button_SS.setText("10");
                else if(bCap.equals("10"))
                    button_SS.setText("100");
                else
                    button_SS.setText("1");
            }
        });
        views_to_fade.add(button_mot = (Button)view.findViewById(R.id.button_motion));
        button_mot.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String bCap = button_mot.getText().toString();
                if(bCap.equals("M=0")) {
                    button_mot.setText("M=1");
                    decoderThread.SetMotion(1);
                }
                else {
                    button_mot.setText("M=0");
                    decoderThread.SetMotion(0);
                }
            }
        });

		messageView = (TextView)view.findViewById(R.id.video_message);
		messageView.setTextColor(App.getClr(R.color.good_text));
		messageView.setText(R.string.initializing_video);

        framesView = (TextView)view.findViewById(R.id.frames_cnt);
        framesView.setText("frames_cnt");
        framesView.setVisibility(View.VISIBLE);

        views_to_fade.add(textViewISO = (TextView)view.findViewById(R.id.iso));
        textViewISO.setVisibility(View.VISIBLE);

		// set the texture listener
		textureView = (ZoomPanTextureView)view.findViewById(R.id.video_surface);
		textureView.setSurfaceTextureListener(this);
		textureView.setZoomRange(MIN_ZOOM, MAX_ZOOM);
		textureView.setOnTouchListener(new View.OnTouchListener()
		{
			@Override
			public boolean onTouch(View v, MotionEvent e)
			{
				switch (e.getAction())
				{
					case MotionEvent.ACTION_DOWN:
						stopFadeOutTimer();
						break;
					case MotionEvent.ACTION_UP:
						if (e.getPointerCount() == 1)
						{
							startFadeOutTimer(false);
						}
						break;
				}
				return false;
			}
		});

        views_to_fade.add(seekBar_iso = (SeekBar)view.findViewById(R.id.seekBar_iso));
		seekBar_iso.setOnSeekBarChangeListener(this);

        views_to_fade.add(seekBar_ss = (SeekBar)view.findViewById(R.id.seekBar_ss));
		seekBar_ss.setOnSeekBarChangeListener(this);

		return view;
	}

	//******************************************************************************
	// onAttach
	//******************************************************************************
	@Override
	public void onAttach(Context context)
	{
		super.onAttach(context);
		try
		{
			Activity activity = (Activity) context;
			fadeListener = (OnFadeListener) activity;
		}
		catch (ClassCastException e)
		{
			throw new ClassCastException(context.toString() + " must implement OnFadeListener");
		}
	}

	//******************************************************************************
	// onDestroy
	//******************************************************************************
	@Override
	public void onDestroy()
	{
		super.onDestroy();
		finishHandler.removeCallbacks(finishRunner);
	}

	//******************************************************************************
	// onStart
	//******************************************************************************
	@Override
	public void onStart()
	{
		super.onStart();

		// create the decoderThread thread
		decoderThread = new DecoderThread();
		//decoderThread.start();
	}

	//******************************************************************************
	// onStop
	//******************************************************************************
	@Override
	public void onStop()
	{
		super.onStop();

		if (decoderThread != null)
		{
			decoderThread.interrupt();
			decoderThread = null;
		}
	}

	//******************************************************************************
	// onPause
	//******************************************************************************
	@Override
	public void onPause()
	{
		super.onPause();
		stopFadeOutTimer();
	}


	//******************************************************************************
	// onSurfaceTextureAvailable
	//******************************************************************************
	@Override
	public void onSurfaceTextureAvailable(SurfaceTexture surfaceTexture, int width, int height)
	{
		if (decoderThread != null)
		{
			decoderThread.setSurface(new Surface(surfaceTexture), startVideoHandler, startVideoRunner, fadeOutHandler, fadeOutRunner);
			decoderThread.start();
		}
	}

	//******************************************************************************
	// onSurfaceTextureSizeChanged
	//******************************************************************************
	@Override
	public void onSurfaceTextureSizeChanged(SurfaceTexture surfaceTexture, int width, int height)
	{
	}

	//******************************************************************************
	// onSurfaceTextureDestroyed
	//******************************************************************************
	@Override
	public boolean onSurfaceTextureDestroyed(SurfaceTexture surfaceTexture)
	{
		if (decoderThread != null)
		{
			decoderThread.setSurface(null, null, null, null, null);
		}
		return true;
	}

	//******************************************************************************
	// onSurfaceTextureUpdated
	//******************************************************************************
	@Override
	public void onSurfaceTextureUpdated(SurfaceTexture surfaceTexture)
	{
	}

	//******************************************************************************
	// startFadeIn
	//******************************************************************************
	public void startFadeIn()
	{
		stopFadeOutTimer();
		fadeInHandler.removeCallbacks(fadeInRunner);
		fadeInHandler.post(fadeInRunner);
		startFadeOutTimer(true);
	}

	private void startFadeOutTimer(boolean addFadeInTime)
	{
		fadeOutHandler.removeCallbacks(fadeOutRunner);
		fadeOutHandler.postDelayed(fadeOutRunner, FADEOUT_TIMEOUT + (addFadeInTime ? FADEIN_ANIMATION_TIME : 0));
	}

	//******************************************************************************
	// stopFadeOutTimer
	//******************************************************************************
	private void stopFadeOutTimer()
	{
		fadeOutHandler.removeCallbacks(fadeOutRunner);
	}

	////////////////////////////////////////////////////////////////////////////////
	// DecoderThread
	////////////////////////////////////////////////////////////////////////////////
	private class DecoderThread extends Thread
	{
		// local constants
		private final static String TAG = "DecoderThread";
		private final static int BUFFER_TIMEOUT = 100000;
		private final static int FINISH_TIMEOUT = 0;
		private final static int TCPIP_BUFFER_SIZE = 2000000;

		// instance variables
		private MediaCodec mediaCodec = null;
		private MediaFormat outputFormat;
		private boolean decoding = false;
		private Surface surface;
		private Source source = null;
		private byte[] buffer = null;
		private Handler startVideoHandler;
		private Runnable startVideoRunner;
        private Handler startFadeoutHandler;
        private Runnable startFadeoutRunner;

		private Socket socket = null;
		private InputStream inputStream = null;
		private PrintWriter sockPrintWriter = null;

		public void SetISO(int iISO)
		{
			if(socket != null && socket.isConnected())
				sockPrintWriter.println(String.format("iso=%d", iISO));
		}

		public void SetSS(int iSS)
		{
			if(socket != null && socket.isConnected())
				sockPrintWriter.println(String.format("ss=%d", iSS));
		}

        public void ZoomIN()
        {
            if(socket != null && socket.isConnected())
                sockPrintWriter.println("move=i");
        }

        public void ZoomOUT()
        {
            if(socket != null && socket.isConnected())
                sockPrintWriter.println("move=o");
        }

        public void SetMotion(int i)
        {
            if(socket != null && socket.isConnected())
                sockPrintWriter.println(String.format("motion=%d", i));
        }

        public void Move(char ch)
        {
            if(socket != null && socket.isConnected())
                sockPrintWriter.println(String.format("move=%c", ch));
        }

		public void setSurface(Surface surface, Handler handler, Runnable runner, Handler fadeout_handler, Runnable fadeout_runner)
		{
			this.surface = surface;
			this.startVideoHandler = handler;
			this.startVideoRunner = runner;
            this.startFadeoutHandler = fadeout_handler;
            this.startFadeoutRunner = fadeout_runner;

			if (mediaCodec == null)
			{
				try {
					outputFormat = MediaFormat.createVideoFormat("video/avc", 1920, 1080);
					mediaCodec = MediaCodec.createDecoderByType("video/avc");
					mediaCodec.configure(outputFormat, surface, null, 0);
				}
				catch (Exception ex){}

				if (surface != null)
				{
					boolean newDecoding = decoding;
					if (decoding)
					{
						setDecodingState(false);
					}
					if (outputFormat != null)
					{
						if (!newDecoding)
						{
							newDecoding = true;
						}
					}
					if (newDecoding)
					{
						setDecodingState(newDecoding);
					}
				}
				else if (decoding)
				{
					setDecodingState(false);
				}
			}
			else if (decoding)
			{
				setDecodingState(false);
			}
		}

		//******************************************************************************
		// getMediaFormat
		//******************************************************************************
		public MediaFormat getMediaFormat()
		{
			return outputFormat;
		}

		//******************************************************************************
		// setDecodingState
		//******************************************************************************
		private synchronized void setDecodingState(boolean newDecoding)
		{
			try
			{
				if (newDecoding != decoding && mediaCodec != null)
				{
					if (newDecoding)
					{
						mediaCodec.start();
					}
					else
					{
						mediaCodec.stop();
					}
					decoding = newDecoding;
				}
			} catch (Exception ex) {}
		}


		private int byteArrayToInt(byte[] b)
		{
			return   b[0] & 0xFF |
					(b[1] & 0xFF) << 8 |
					(b[2] & 0xFF) << 16 |
					(b[3] & 0xFF) << 24;
		}


		private Boolean read_exact(byte[] buffer, int cnt)
		{
			if(inputStream == null)
				return false;
            int iToRead = cnt;
            int offset = 0;
			try
			{
				int read;
				while(iToRead > 0)
				{
					read = inputStream.read(buffer, offset, iToRead);
                    if(read < 1)
                        throw new Exception("qwer");
					iToRead -= read;
					offset += read;
				}
				return true;
			}
			catch (Exception ex)
			{
                setMessage(String.format("read_exact(cnt=%d, offset=%d, iToRead=%d)", cnt, offset, iToRead));
				return false;
			}
		}

        int g_iFrames_cnt = 0;
        int g_iFrames_cnt_tmp = 0;
        long lastFpsTime = System.currentTimeMillis();
        Handler timerHandler = new Handler();
        Runnable timerRunnable = new Runnable() {

            @Override
            public void run()
            {
                if(getActivity() != null) {
                    long timeNow = System.currentTimeMillis();
                    final long iTimeDiff = timeNow - lastFpsTime;
                    lastFpsTime = timeNow;
                    final int fr_diff = g_iFrames_cnt - g_iFrames_cnt_tmp;
                    final int fr_cnt = g_iFrames_cnt;
                    g_iFrames_cnt_tmp = g_iFrames_cnt;
                    if(iTimeDiff > 0)
                    {
                        getActivity().runOnUiThread(new Runnable() {
                            public void run() {
                                framesView.setText(String.format("%d, %d, FPS=%d", fr_cnt, iTimeDiff, Math.round((1000*fr_diff)/iTimeDiff)));
                            }
                        });
                    }
                    timerHandler.postDelayed(this, 1000);
                }
            }
        };

		@Override
		public void run()
		{
			long presentationTime = System.nanoTime() / 1000;

			try
			{
				source = camera.getCombinedSource();

				InetSocketAddress socketAddress = new InetSocketAddress(source.address, source.port);

				boolean bConnected = false;
				int attempts = 0;
				while(!bConnected) {
					try {
						setMessage(String.format("connecting(%d) to %s:%d...", attempts++, source.address, source.port));
						socket = new Socket();
						socket.connect(socketAddress, 3000);
						inputStream = socket.getInputStream();
						sockPrintWriter = new PrintWriter(socket.getOutputStream(), true);
						bConnected = true;
					} catch (Exception ex) {
						/*StringWriter sw = new StringWriter();
						ex.printStackTrace(new PrintWriter(sw));*/
						setMessage(ex.toString());
						sleep(3000, 0);
						socket = null;
						if(Thread.interrupted())
							return;
					}
				}
                timerHandler.postDelayed(timerRunnable, 0);//run show FPS thread

                buffer = new byte[TCPIP_BUFFER_SIZE];

                startVideoHandler.post(startVideoRunner);
                startFadeoutHandler.post(startFadeoutRunner);

				//read and set SPS, PPS
				for(int i = 0; i < 2; i++) {
					if (!read_exact(buffer, 4))
                        throw new Exception("read_exact");

					int sps_or_pps_len = byteArrayToInt(buffer);
					if (!read_exact(buffer, sps_or_pps_len))
                        throw new Exception("read_exact");

					int inputBufferId = mediaCodec.dequeueInputBuffer(BUFFER_TIMEOUT);
					if (inputBufferId >= 0) {
						ByteBuffer inputBuffer = mediaCodec.getInputBuffer(inputBufferId);
						inputBuffer.put(buffer, 0, sps_or_pps_len);
						mediaCodec.queueInputBuffer(inputBufferId, 0, sps_or_pps_len, presentationTime, MediaCodec.BUFFER_FLAG_CODEC_CONFIG);
						presentationTime += 666666;
					} else {
						setMessage(String.format("codec.dequeueInputBuffer=%d", inputBufferId));
						return;
					}
				}

				MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
				int outputBufferId = mediaCodec.dequeueOutputBuffer(info, BUFFER_TIMEOUT);
				if (outputBufferId >= 0) {
					mediaCodec.releaseOutputBuffer(outputBufferId, true);
				} else if (outputBufferId == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
					// Subsequent data will conform to new format.
					// Can ignore if using getOutputFormat(outputBufferId)
					outputFormat = mediaCodec.getOutputFormat(); // option B
				}

                setMessage("");
				while (!Thread.interrupted())
				{
					if (!read_exact(buffer, 4))
                        throw new Exception("read_exact");

					int iFrameLen = byteArrayToInt(buffer);
					if(iFrameLen > TCPIP_BUFFER_SIZE)
					{
						setMessage(String.format("iFrameLen=%d", iFrameLen));
						return;
					}

					int inputBufferId = mediaCodec.dequeueInputBuffer(BUFFER_TIMEOUT);
					if (inputBufferId >= 0) {
						ByteBuffer inputBuffer = mediaCodec.getInputBuffer(inputBufferId);
                        if (!read_exact(buffer, iFrameLen))
                            throw new Exception("read_exact");
						inputBuffer.put(buffer, 0, iFrameLen);
						mediaCodec.queueInputBuffer(inputBufferId, 0, iFrameLen, presentationTime, 0);
						presentationTime += 666666;
					} else {
						setMessage(String.format("codec.dequeueInputBuffer=%d", inputBufferId));
						return;
					}
					outputBufferId = mediaCodec.dequeueOutputBuffer(info, BUFFER_TIMEOUT);
					if (outputBufferId >= 0) {
						mediaCodec.releaseOutputBuffer(outputBufferId, true);
					} else if (outputBufferId == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
						// Subsequent data will conform to new format.
						// Can ignore if using getOutputFormat(outputBufferId)
						outputFormat = mediaCodec.getOutputFormat(); // option B
					}

                    g_iFrames_cnt++;
                    /*final int fr_cnt = g_iFrames_cnt;
                    getActivity().runOnUiThread(new Runnable()
                    {
                        public void run()
                        {
                            framesView.setText(String.format("%d", fr_cnt));
                        }
                    });*/
				}
			}
			catch (Exception ex)
			{
                finishHandler.postDelayed(finishRunner, FINISH_TIMEOUT);
			}

			try {
				if (inputStream != null) {
					inputStream.close();
					inputStream = null;
				}
				if (socket != null) {
					socket.close();
					socket = null;
				}
			}catch (Exception ex) {}


			// stop the decoderThread
			if (mediaCodec != null)
			{
				try
				{
					setDecodingState(false);
					mediaCodec.release();
				}
				catch (Exception ex) {}
				mediaCodec = null;
			}
		}

		//******************************************************************************
		// hideMessage
		//******************************************************************************
		private void hideMessage()
		{
			getActivity().runOnUiThread(new Runnable()
			{
				public void run()
				{
					messageView.setVisibility(View.GONE);
				}
			});
		}
		private void setMessage(final String str)
		{
			getActivity().runOnUiThread(new Runnable()
			{
				public void run()
				{
					messageView.setText(str);
					messageView.setVisibility(View.VISIBLE);
				}
			});
		}
	}
}
