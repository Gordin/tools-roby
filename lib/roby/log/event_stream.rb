require 'roby/log/data_stream'

module Roby
    module Log
	# This class is a logger-compatible interface which read event and index logs,
	# and may rebuild the task and event graphs from the marshalled events
	# that are saved using for instance FileLogger
	class EventStream < DataStream
	    def splat?; true end

	    # The event log
	    attr_reader :logfile

	    # The index of the currently displayed cycle in +index_data+
	    attr_reader :current_cycle
	    # A [min, max] array of the minimum and maximum times for this
	    # stream
	    def range; logfile.range end

	    def initialize(basename)
		@logfile = Roby::Log.open("#{basename}-events.log")

		super(basename, 'roby-events')
		reinit!
	    end

	    def index_data; logfile.index_data end

	    # True if the stream has been reinitialized
	    def reinit?
		@reinit ||= (!index_data.empty? && logfile.stat.size < index_data.last[:pos])
	    end

	    # Reinitializes the stream
	    def reinit!
		@current_cycle = 0
		prepare_seek(nil)

		super
	    end

	    # True if there is at least one sample available
	    def has_sample?
		update_index
		!index_data.empty? && (index_data.last[:pos] > logfile.tell)
	    end

	    # Seek the data stream to the specified time.
	    def prepare_seek(time)
		if !time || !current_time || time < current_time
		    clear

		    @current_time  = nil
		    @current_cycle = 0
		    logfile.rewind
		end
	    end
	    
	    # The current time
	    def current_time
		return if index_data.empty?
		if index_data.size == current_cycle + 1
		    index_data[current_cycle][:end]
		else
		    index_data[current_cycle][:start]
		end
	    end

	    # The time we will reach when the next sample is processed
	    def next_time
		return if index_data.empty?
		if index_data.size > current_cycle + 1
		    index_data[current_cycle + 1][:start]
		end
	    end

	    # Reads a sample of data and returns it. It will be fed to
	    # decoders' #decode method.
	    #
	    # In this stream, this is the chunk of the marshalled file which
	    # corresponds to a cycle
	    def read
		if reinit?
		    reinit!
		end

		start_pos = index_data[current_cycle][:pos]
		end_pos   = if index_data.size > current_cycle + 1
				index_data[current_cycle + 1][:pos]
			    else
				logfile.stat.size
			    end

		logfile.seek(start_pos)
		logfile.read(end_pos - start_pos)

	    ensure
		@current_cycle += 1
	    end

	    # Unmarshalls a set of data returned by #read_all and yield
	    # each sample that should be fed to the decoders
	    def self.init(data)
		io = StringIO.new(data)
		while !io.eof?
		    yield(Marshal.load(io))
		end
	    end

	    # Unmarshalls one cycle of data returned by #read and feeds
	    # it to the decoders
	    def self.decode(data)
		io = StringIO.new(data)
		Marshal.load(io)
	    end

	    # Read all data read so far in a format suitable to feed to
	    # #init_stream on the decoding side
	    def read_all
		end_pos   = if index_data.size > current_cycle + 1
				index_data[current_cycle + 1][:pos]
			    else
				logfile.stat.size
			    end
		logfile.seek(0)
		logfile.read(end_pos)
	    end
	end
    end
end
