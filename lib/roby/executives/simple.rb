module Roby
    module Executives
	class Simple
	    attr_reader :query
	    def initialize
		@query = Roby.plan.find_tasks.
		    executable.
		    pending
	    end
	    def initial_events
		query.reset.each do |task|
		    next unless task.event(:start).root?
		    root_task = task.enum_for(:each_relation).all? do |rel|
			rel == TaskStructure::PlannedBy || task.root?(rel)
		    end

		    if root_task
			task.start!
		    end
		end
	    end
	end
    end
end
