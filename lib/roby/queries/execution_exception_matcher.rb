module Roby
    module Queries
        # Object that allows to specify generalized matches on a Roby::ExecutionException
        # object
        class ExecutionExceptionMatcher
            # An object that will be used to match the actual (Ruby) exception
            # @return [LocalizedErrorMatcher]
            attr_reader :exception_matcher
            # An object that will be used to match the exception origin
            # @return [Array<#===,TaskMatcher>]
            attr_reader :involved_tasks_matchers

            def initialize
                @exception_matcher = LocalizedErrorMatcher.new
                @involved_tasks_matchers = Array.new
            end

            # Sets the exception matcher object
            def with_exception(exception_matcher)
                @exception_matcher = exception_matcher
                self
            end

            # (see LocalizedErrorMatcher#with_model)
            def with_model(exception_model)
                exception_matcher.with_model(exception_model)
                self
            end

            # (see LocalizedErrorMatcher#with_origin)
            def with_origin(plan_object_matcher)
                exception_matcher.with_origin(plan_object_matcher)
                self
            end

            # (see LocalizedErrorMatcher#fatal)
            def fatal
                exception_matcher.fatal
                self
            end

            # Matched exceptions must have a task in their trace that matches
            # the given task matcher
            #
            # @param [TaskMatcher] task_matcher
            def involving(task_matcher)
                involved_tasks_matchers << origin_matcher
                self
            end

            def to_s
                "#{exception_matcher}.involving(#{involved_tasks_matchers.map(&:to_s).join(", ")})"
            end

            # @return [Boolean] true if the given execution exception object
            #   matches self, false otherwise
            def ===(exception)
                exception_matcher === exception.exception &&
                    involved_tasks_matchers.all? { |m| exception.trace.any? { |t| m === t } }
            end

            def matches_task?(task)
                involved_tasks_matchers.all? { |m| m === task } &&
                    exception_matcher.matches_task?(task)
            end

            def to_execution_exception_matcher
                self
            end

            # An intermediate representation of OrMatcher objects suitable to
            # be sent to our peers.
            class DRoby
                attr_reader :exception_matcher, :involved_tasks_matchers
                def initialize(exception_matchers, involved_tasks_matchers)
                    @exception_matcher = exception_matcher
                    @involved_tasks_matchers = involved_tasks_matchers
                end
                def proxy(peer)
                    matcher = ExecutionExceptionMatcher.new
                    matcher.with_exception(peer.local_object(exception_matcher))
                    involved_tasks_matchers.each do |m|
                        matcher.involving(peer.local_object(m))
                    end
                    matcher
                end
            end
            
            # Returns an intermediate representation of +self+ suitable to be sent
            # to the +dest+ peer.
            def droby_dump(dest)
                DRoby.new(exception_matcher.droby_dump(dest), involved_tasks_matchers.droby_dump(dest))
            end
        end
    end
end
